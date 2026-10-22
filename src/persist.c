/* persist.c - Pluggable Persistence Framework for Linear Data Structures
 *
 * See persist.h for API documentation and architecture overview.
 *
 * Implementation follows datakit design principles:
 * - Self-managing memory (zmalloc/zfree)
 * - Linear memory-efficient structures
 * - Clean encapsulation with opaque types
 * - Comprehensive test coverage
 *
 * SCALABILITY NOTES:
 *
 * Current implementations use non-streaming snapshot/restore which buffers
 * the entire serialized structure in memory. This is efficient for structures
 * up to several GB. For terabyte-scale data (e.g., multilistFull with billions
 * of entries), use the streaming API:
 *
 * 1. streamSnapshot - Writes data in chunks via callback, never buffering
 * the entire structure. For multilist: iterate nodes, serialize each
 * node independently, call callback per node.
 *
 * 2. streamRestore - Reads data in chunks via callback, building structure
 * incrementally. For multilist: receive node data via callback, append
 * each node to growing structure.
 *
 * The streaming callbacks are defined in persistOps but implementations are
 * marked NULL for the current non-streaming approach. When streaming is needed,
 * implement streamSnapshot/streamRestore for the relevant data type.
 *
 * For extremely large structures, also consider:
 * - Incremental snapshots (delta encoding between snapshots)
 * - Parallel restore (multi-threaded node reconstruction)
 * - Memory-mapped I/O for the storage backend
 */

#include "persist.h"
#include "cleaner.h"
#include "databoxLinear.h"
#include "datakit.h"
#include "flexEnvelope.h"
#include "hmacKeyring.h"
#include "mflex.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(flex *, flexFree);
defcleanerName(uint8_t *, zfree, Bytes);
defcleanerName(char *, zfree, Chars);
defcleanerName(bool *, zfree, Bools);
defcleaner(mflexState *, mflexStateFree);

/* Scope-exit cleaner for the fixed 2-slot mflex state array the read-only
 * multilist iterator requires: frees whichever slots were created. */
static DK_FN_UNUSED void freeMflexStates2_cleanup(mflexState *(*arr)[2]) {
    for (size_t i = 0; i < 2; i++) {
        if ((*arr)[i]) {
            mflexStateFree((*arr)[i]);
        }
    }
}

#ifndef XXH_STATIC_LINKING_ONLY
#define XXH_STATIC_LINKING_ONLY
#endif
#include "../deps/xxHash/xxhash.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef DATAKIT_TEST
#include "perf.h"
#endif

/* The on-disk header structs below are packed and written/read raw — no
 * per-field byte-swap. That is correct on little-endian targets (every
 * platform datakit currently runs on) but silently produces an
 * unportable file on a big-endian host. Pin the assumption at compile
 * time so a BE port is forced to confront it explicitly. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "persist.c on-disk format assumes little-endian; "
               "big-endian hosts need explicit byte-swap helpers");
#endif

/* ============================================================================
 * Magic Numbers and Format Constants
 * ============================================================================
 */
#define PERSIST_SNAP_MAGIC ((uint32_t)0x50534B44) /* "DKSP" little-endian */
#define PERSIST_WAL_MAGIC ((uint32_t)0x4C574B44)  /* "DKWL" little-endian */
/* Version 2 adds a `keyId` field to both snap+WAL headers.
 * No backward compat with v1: pre-release software, readers reject the
 * old size+digest layout outright via the version check. */
#define PERSIST_VERSION ((uint16_t)3)

#define PERSIST_SNAP_HEADER_SIZE ((size_t)48)
#define PERSIST_WAL_HEADER_SIZE ((size_t)28)

/* Header self-check digest sizes (raw XXH32/XXH64 on the persist header
 * itself). The full body / entry integrity is carried by the envelope. */
#define PERSIST_SNAP_HEADER_DIGEST_LEN ((size_t)8)
#define PERSIST_SNAP_HEADER_PREFIX_LEN                                         \
    (PERSIST_SNAP_HEADER_SIZE - PERSIST_SNAP_HEADER_DIGEST_LEN)
#define PERSIST_WAL_HEADER_DIGEST_LEN ((size_t)4)
#define PERSIST_WAL_HEADER_PREFIX_LEN                                          \
    (PERSIST_WAL_HEADER_SIZE - PERSIST_WAL_HEADER_DIGEST_LEN)

/* Upper bound on a single WAL entry envelope. Same value the old code
 * used; protects readers from a bogus length field forcing a huge
 * allocation before envelope verify can reject it. */
#define PERSIST_WAL_ENTRY_MAX_BYTES ((uint32_t)(100u * 1024u * 1024u))

/* ============================================================================
 * Persist Context
 * ============================================================================
 */
struct persist {
    const persistOps *ops;       /* Structure-specific operations */
    persistConfig config;        /* Configuration */
    persistStore *snapshotStore; /* Snapshot storage backend */
    persistStore *walStore;      /* WAL storage backend */
    persistStats stats;          /* Statistics */

    /* Borrowed integrity key for keyed-class checksum algorithms. Lifetime
     * is the caller's responsibility — see persistSetIntegrityKey in
     * persist.h. NULL/0 when no key has been set.
     *
     * IMPORTANT: when persistAttachKeyring/persistRebindKeyring has been
     * called, these raw fields are NOT used — every wrap/verify call
     * resolves the live key via persistResolveAttachedKey (which
     * checks the generation counter and returns NULL on rotation).
     * They remain in use only when the caller went through the legacy
     * persistSetIntegrityKey API directly. */
    const uint8_t *integrityKey;
    size_t integrityKeyLen;

    /* Optional borrowed prepared-key handle (hmacKeyring or hand-crafted
     * via hmacPrepareKey). When non-NULL AND the configured algorithm
     * is HMAC-class, every verify call uses the prepared state directly,
     * skipping the two K_ipad/K_opad block compressions per envelope.
     * This is the hot-path acceleration for WAL replay / cluster apply
     * mailbox. Ignored for BLAKE3_KEYED (no prepared form). Same note
     * as integrityKey above: bypassed when an attached keyring binding
     * is live. */
    const struct hmacKey *integrityPreparedKey;

    /* Attached keyring binding. When `attachedKeyring != NULL` the
     * persist instance is in "attach mode" and resolves the integrity
     * key on every operation via the keyring's generation API. See
     * persistAttachKeyring and persistResolveAttachedKey. The
     * cached generation is the value observed at attach time; any
     * mismatch on resolve indicates the slot was rotated since attach
     * and the operation MUST fail closed rather than use the (possibly
     * freed) stale pointers. */
    struct hmacKeyring *attachedKeyring; /* borrowed; never freed by us */
    uint32_t attachedKeyId;
    uint64_t attachedGeneration;

    /* Sticky typed-error slot. Updated by failure paths inside
     * persistResolveAttachedKey so callers can distinguish "key
     * rotated" from other failure modes via persistLastError. Reset
     * to PERSIST_ERR_NONE by successful operations on the attached
     * keyring path. */
    persistErr lastError;

    /* When a snapshot/WAL verify fails with PERSIST_ERR_KEY_ID_MISMATCH
     * this slot holds the key_id the on-disk header recorded. Cleared
     * to zero on success and on errors that aren't KEY_ID_MISMATCH.
     * Exposed via persistLastExpectedKeyId so the operator can decide
     * which key to rotate the reader to. */
    uint32_t lastExpectedKeyId;

    /* WAL state */
    uint64_t walSequence; /* Next sequence number */
    /* WAL sequence the restored snapshot already includes; replay
     * skips entries below it so a stale post-crash WAL can't
     * double-apply ops already folded into the snapshot. 0 when no
     * snapshot was restored (a WAL-only replay applies everything). */
    uint64_t snapshotWalSequence;
    uint8_t *walBuffer;    /* Write buffer */
    size_t walBufferUsed;  /* Bytes used in buffer */
    uint64_t lastSyncTime; /* Last sync timestamp (microseconds) */
    bool walInitialized;   /* Whether WAL header has been written */
};

/* ============================================================================
 * Algorithm Mapping
 * ============================================================================
 * persistChecksum is the user-facing knob; flexEnvelopeAlgo is the on-wire
 * identifier the envelope writes into its framing. These two enums share
 * the same membership but are kept separate so persistChecksum can be
 * documented and validated independently. */

static flexEnvelopeAlgo persistChecksumToEnvelopeAlgo(persistChecksum t) {
    switch (t) {
    case PERSIST_CHECKSUM_NONE:
        return FLEX_ENV_ALGO_NONE;
    case PERSIST_CHECKSUM_XXH64:
        return FLEX_ENV_ALGO_XXH64;
    case PERSIST_CHECKSUM_XXH3_64:
        return FLEX_ENV_ALGO_XXH3_64;
    case PERSIST_CHECKSUM_XXH3_128:
        return FLEX_ENV_ALGO_XXH3_128;
    case PERSIST_CHECKSUM_BLAKE3_KEYED:
        return FLEX_ENV_ALGO_BLAKE3_KEYED;
    case PERSIST_CHECKSUM_HMAC_SHA256:
        return FLEX_ENV_ALGO_HMAC_SHA256;
    case PERSIST_CHECKSUM_HMAC_BLAKE3:
        return FLEX_ENV_ALGO_HMAC_BLAKE3;
    }
    return FLEX_ENV_ALGO_NONE;
}

/* Compute the persist-header self-check digest (XXH64 over the header
 * prefix). Used identically by writer and reader. */
static uint64_t persistSnapHeaderDigest(const void *headerBytes) {
    return (uint64_t)XXH64(headerBytes, PERSIST_SNAP_HEADER_PREFIX_LEN, 0);
}

static uint32_t persistWalHeaderDigest(const void *headerBytes) {
    return (uint32_t)XXH32(headerBytes, PERSIST_WAL_HEADER_PREFIX_LEN, 0);
}

/* Build the envelope wrap params for a persist's current configuration.
 * Returns true on success. Returns false if the configured algorithm is
 * keyed-class but no integrity key has been set (would silently produce
 * an unverifiable artifact otherwise), or if the attached-keyring
 * binding (if any) has been invalidated by a rotation since attach —
 * see persistResolveAttachedKey for the typed-error contract.
 *
 * Takes a non-const persist* because the attached-keyring path mutates
 * the cached integrityKey pointers + lastError as part of resolve. */
static bool persistResolveAttachedKey(persist *p);

static bool persistBuildWrapParams(persist *p,
                                   flexEnvelopeWrapParams *outParams,
                                   flexEnvelopeAlgo *outAlgo) {
    const flexEnvelopeAlgo algo =
        persistChecksumToEnvelopeAlgo(p->config.checksumType);
    /* When a keyring is attached, re-resolve every time so a Retire that
     * happened between two snapshot calls is caught here rather than
     * silently HMAC-ing with the freed (and zero-wiped) key bytes. The
     * resolver sets p->lastError on failure so the caller can distinguish
     * "key rotated" from other false returns. */
    if (p->attachedKeyring != NULL) {
        if (!persistResolveAttachedKey(p)) {
            return false;
        }
    }
    if (flexEnvelopeAlgoIsKeyed(algo) &&
        (p->integrityKey == NULL || p->integrityKeyLen == 0)) {
        p->lastError = PERSIST_ERR_NO_KEY_ATTACHED;
        return false;
    }
    *outAlgo = algo;
    *outParams = (flexEnvelopeWrapParams){
        .key = p->integrityKey,
        .key_len = p->integrityKeyLen,
        .algo = algo,
        .on_wire_flags = FLEX_ENV_FLAG_NONE,
        .version = FLEX_ENVELOPE_VERSION_CURRENT,
    };
    return true;
}

/* Build verify params. Returns true on success, false if an attached-
 * keyring binding has been invalidated. Like persistBuildWrapParams
 * this takes non-const `persist *` because the resolver mutates cached
 * pointers + lastError. */
static bool persistBuildVerifyParams(persist *p,
                                     flexEnvelopeVerifyParams *outParams) {
    if (p->attachedKeyring != NULL) {
        if (!persistResolveAttachedKey(p)) {
            return false;
        }
    }
    *outParams = (flexEnvelopeVerifyParams){
        .max_payload_len = FLEX_ENVELOPE_DEFAULT_MAX_PAYLOAD,
        .key = p->integrityKey,
        .key_len = p->integrityKeyLen,
        .prepared_key = p->integrityPreparedKey,
        .opts = FLEX_ENV_VERIFY_DEFAULT,
    };
    return true;
}

/* Compare on-disk header key_id against the currently-attached keyring's
 * key_id. Returns true if the verify can proceed (no mismatch); false
 * if the reader is pointed at the wrong key. On false return, sets
 * p->lastError = PERSIST_ERR_KEY_ID_MISMATCH and stashes the header's
 * expected key_id into p->lastExpectedKeyId so the operator can read it
 * back via persistLastExpectedKeyId.
 *
 * Semantics:
 * - If no keyring is attached, accept any header (legacy direct-key
 * callers still work; they get plain VERIFY_FAILED on key mismatch).
 * - If the header's key_id is 0 (writer had no keyring attached when
 * it wrote the file), accept — there is nothing to compare against,
 * and the envelope's MAC still gates correctness.
 * - Otherwise the two must match exactly. */
static bool persistCheckHeaderKeyId(persist *p, uint32_t headerKeyId) {
    if (p->attachedKeyring == NULL) {
        return true;
    }
    if (headerKeyId == 0u) {
        /* Writer had no keyring attached; treat as a "key_id-absent"
         * header. The envelope MAC is still required to verify, so a
         * cross-key paste-attack would still be caught — just later
         * and as a generic VERIFY_FAILED instead of the typed early
         * reject. */
        return true;
    }
    if (headerKeyId != (uint32_t)p->attachedKeyId) {
        p->lastError = PERSIST_ERR_KEY_ID_MISMATCH;
        p->lastExpectedKeyId = headerKeyId;
        return false;
    }
    return true;
}

/* ============================================================================
 * Time Utilities
 * ============================================================================
 */
static uint64_t persistGetMicroseconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
}

/* ============================================================================
 * Default Configuration
 * ============================================================================
 */
persistConfig persistDefaultConfig(void) {
    return (persistConfig){
        .compression = PERSIST_COMPRESS_NONE,
        .compressionLevel = 0,
        .checksumType = PERSIST_CHECKSUM_XXH64,
        .syncMode = PERSIST_SYNC_EVERYSEC,
        .walMaxSize = (size_t)64 * 1024 * 1024, /* 64 MB */
        .walBufferSize = (size_t)64 * 1024,     /* 64 KB */
        .autoCompact = true,
        .compactRatio = 2.0,
        .strictRecovery = false,
    };
}

/* ============================================================================
 * Core API Implementation
 * ============================================================================
 */

persist *persistCreate(const persistOps *ops, const persistConfig *config) {
    if (!ops) {
        return NULL;
    }

    /* @own: caller; free=persistClose */
    persist *p = zmalloc(sizeof(*p));
    p->ops = ops;
    p->config = config ? *config : persistDefaultConfig();
    p->snapshotStore = NULL;
    p->walStore = NULL;
    memset(&p->stats, 0, sizeof(p->stats));

    p->integrityKey = NULL;
    p->integrityKeyLen = 0;
    p->integrityPreparedKey = NULL;

    p->attachedKeyring = NULL;
    p->attachedKeyId = 0u;
    p->attachedGeneration = 0u;
    p->lastError = PERSIST_ERR_NONE;
    p->lastExpectedKeyId = 0u;

    p->walSequence = 1;
    p->snapshotWalSequence = 0;
    p->walBuffer = NULL;
    p->walBufferUsed = 0;
    p->lastSyncTime = persistGetMicroseconds();
    p->walInitialized = false;

    /* Allocate WAL buffer if needed */
    if (p->config.walBufferSize > 0) {
        /* @own: container; free=persistClose */
        p->walBuffer = zmalloc(p->config.walBufferSize);
    }

    return p;
}

bool persistAttachSnapshot(persist *p, persistStore *store) {
    if (!p || !store) {
        return false;
    }
    p->snapshotStore = store;
    return true;
}

bool persistAttachWAL(persist *p, persistStore *store) {
    if (!p || !store) {
        return false;
    }
    p->walStore = store;
    return true;
}

bool persistSetIntegrityKey(persist *p, const uint8_t *key, size_t key_len) {
    if (!p) {
        return false;
    }
    /* Direct-key API: detach from any attached keyring binding so the
     * resolver doesn't override the caller's explicit choice. */
    p->attachedKeyring = NULL;
    p->attachedKeyId = 0u;
    p->attachedGeneration = 0u;
    if (key == NULL || key_len == 0) {
        p->integrityKey = NULL;
        p->integrityKeyLen = 0;
        return true;
    }
    p->integrityKey = key;
    p->integrityKeyLen = key_len;
    return true;
}

bool persistSetIntegrityPreparedKey(persist *p, const struct hmacKey *prep) {
    if (!p) {
        return false;
    }
    p->integrityPreparedKey = prep;
    return true;
}

/* Internal: refresh `p`'s cached raw + prepared key from the currently
 * attached keyring slot. Validates that the slot's generation matches
 * the value captured at attach/rebind time; mismatch sets lastError to
 * PERSIST_ERR_KEY_GENERATION_MISMATCH and returns false WITHOUT
 * touching p->integrityKey/integrityPreparedKey (the caller treats this
 * as fail-closed and refuses the operation rather than silently using
 * stale or freed pointers).
 *
 * On success returns true and refreshes integrityKey/integrityKeyLen/
 * integrityPreparedKey to point into the live keyring slot. These
 * pointers are valid for the duration of the immediate operation but
 * the caller MUST re-resolve before each subsequent operation — the
 * keyring's rwlock is released between calls and another writer could
 * retire the slot in the meantime. In practice persistBuildWrapParams
 * and persistBuildVerifyParams call this exactly once per snapshot /
 * WAL append / verify; the resolved pointers feed straight into the
 * envelope call and are not reused. */
static bool persistResolveAttachedKey(persist *p) {
    if (p == NULL || p->attachedKeyring == NULL) {
        return false;
    }
    uint64_t current = 0u;
    if (!hmacKeyringGetGeneration(p->attachedKeyring, p->attachedKeyId,
                                  &current)) {
        /* The id is gone entirely — retired and the slot is free. */
        p->lastError = PERSIST_ERR_KEY_NOT_FOUND;
        return false;
    }
    if (current != p->attachedGeneration) {
        /* Slot has been mutated since attach — retire-then-reinstall,
         * salt rotation, etc. Refuse to use whatever bytes are there
         * now: the caller asked us to hold a specific (id, generation)
         * pair and that contract is broken. */
        p->lastError = PERSIST_ERR_KEY_GENERATION_MISMATCH;
        return false;
    }
    /* Generation matches — resolve the live bytes + prepared handle. */
    const uint8_t *bytes = NULL;
    size_t blen = 0;
    if (!hmacKeyringGetRaw(p->attachedKeyring, p->attachedKeyId, &bytes, &blen,
                           NULL)) {
        /* Should not happen given the generation matched, but treat as
         * the same fail-closed case. */
        p->lastError = PERSIST_ERR_KEY_NOT_FOUND;
        return false;
    }
    p->integrityKey = bytes;
    p->integrityKeyLen = blen;
    p->integrityPreparedKey =
        hmacKeyringLookup(p->attachedKeyring, p->attachedKeyId);
    p->lastError = PERSIST_ERR_NONE;
    p->lastExpectedKeyId = 0u;
    return true;
}

/* Shared body for Attach + Rebind — same machinery, different names so
 * the call site documents intent (first attach vs. post-rotation
 * rebind). */
static bool persistBindKeyringInternal(persist *p, hmacKeyring *ring,
                                       hmacKeyId key_id) {
    if (!p || !ring) {
        return false;
    }
    /* Capture the generation BEFORE resolving the bytes — any mutation
     * that happens between GetGeneration and GetRaw would be observed
     * by a later persistResolveAttachedKey call (the cached gen would
     * mismatch the next gen read). */
    uint64_t gen = 0u;
    if (!hmacKeyringGetGeneration(ring, key_id, &gen)) {
        /* Don't clobber an existing attachment when the call fails;
         * matches the documented "missing id leaves binding intact"
         * semantic the existing test relies on. */
        return false;
    }
    /* Resolve once now to make sure the id is genuinely live AND the
     * algo has a prepared key shape we can use. Refusing here keeps
     * the binding from going into a half-attached state where
     * persistResolveAttachedKey would later succeed but with NULL key
     * bytes (which the envelope would reject anyway, but earlier-is-
     * cheaper). */
    const uint8_t *bytes = NULL;
    size_t blen = 0;
    if (!hmacKeyringGetRaw(ring, key_id, &bytes, &blen, NULL)) {
        return false;
    }
    p->attachedKeyring = ring;
    p->attachedKeyId = key_id;
    p->attachedGeneration = gen;
    p->integrityKey = bytes;
    p->integrityKeyLen = blen;
    p->integrityPreparedKey = hmacKeyringLookup(ring, key_id);
    p->lastError = PERSIST_ERR_NONE;
    p->lastExpectedKeyId = 0u;
    return true;
}

bool persistAttachKeyring(persist *p, hmacKeyring *ring, hmacKeyId key_id) {
    return persistBindKeyringInternal(p, ring, key_id);
}

bool persistRebindKeyring(persist *p, hmacKeyring *ring, hmacKeyId key_id) {
    return persistBindKeyringInternal(p, ring, key_id);
}

persistErr persistLastError(const persist *p) {
    if (p == NULL) {
        return PERSIST_ERR_NONE;
    }
    return p->lastError;
}

uint32_t persistLastExpectedKeyId(const persist *p) {
    if (p == NULL) {
        return 0u;
    }
    return p->lastExpectedKeyId;
}

void persistSetConfig(persist *p, const persistConfig *config) {
    if (p && config) {
        /* Check if buffer size is changing before updating config */
        size_t oldBufferSize = p->config.walBufferSize;
        p->config = *config;

        /* Reallocate WAL buffer if size changed */
        if (oldBufferSize != config->walBufferSize) {
            zfree(p->walBuffer);
            /* @own: container; free=persistClose */
            p->walBuffer = config->walBufferSize > 0
                               ? zmalloc(config->walBufferSize)
                               : NULL;
            p->walBufferUsed = 0;
        }
    }
}

void persistGetConfig(const persist *p, persistConfig *config) {
    if (p && config) {
        *config = p->config;
    }
}

void persistGetStats(const persist *p, persistStats *stats) {
    if (!stats) {
        return;
    }

    /* The void return cannot report a NULL persist, so the caller's
     * struct is always written: zeros when there is nothing to read. */
    if (!p) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    *stats = p->stats;
    stats->walSequence = p->walSequence;
}

void persistResetStats(persist *p) {
    if (p) {
        memset(&p->stats, 0, sizeof(p->stats));
    }
}

void persistClose(persist *p) {
    if (!p) {
        return;
    }

    /* Flush any buffered WAL data */
    persistSync(p);

    /* Close storage backends */
    if (p->snapshotStore && p->snapshotStore->close) {
        p->snapshotStore->close(p->snapshotStore->ctx);
        zfree(p->snapshotStore);
    }
    if (p->walStore && p->walStore->close) {
        p->walStore->close(p->walStore->ctx);
        zfree(p->walStore);
    }

    zfree(p->walBuffer);
    zfree(p);
}

/* ============================================================================
 * Snapshot Operations
 * ============================================================================
 */

/* Snapshot header layout (48 bytes, packed; see persist.h for prose).
 *
 * 0-3: magic (PERSIST_SNAP_MAGIC, "DKSP")
 * 4-5: version (PERSIST_VERSION)
 * 6-7: flags (reserved, must be 0 — readers reject unknown bits)
 * 8-11: structType (must match p->ops->type at restore)
 * 12-19: count (element count reported by p->ops->count)
 * 20-27: envelopeLen (byte length of the body envelope that follows)
 * 28-31: keyId (attached keyring key_id; 0 = none)
 * 32-39: walSequence (WAL sequence this snapshot reflects; replay skips
 *        lower-sequenced ops — see the struct field)
 * 40-47: headerXxh64 (XXH64 over bytes 0..39 — see persistSnapHeaderDigest) */
typedef struct __attribute__((packed)) persistSnapHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t structType;
    uint64_t count;
    uint64_t envelopeLen;
    uint32_t keyId;
    /* WAL sequence this snapshot already reflects: every WAL op with a
     * lower sequence is folded in, so replay skips them. This is what
     * makes a snapshot-then-truncate compaction crash-consistent - a
     * crash between the atomic snapshot publish and the WAL truncate
     * leaves stale WAL entries that all sort below this and are
     * skipped rather than double-applied. */
    uint64_t walSequence;
    uint64_t headerXxh64;
} persistSnapHeader;

DK_SIZECHECK(persistSnapHeader, PERSIST_SNAP_HEADER_SIZE);

bool persistSnapshot(persist *p, const void *structure) {
    if (!p || !structure || !p->snapshotStore || !p->ops->snapshot) {
        return false;
    }

    flexEnvelopeWrapParams wrap;
    flexEnvelopeAlgo algo;
    if (!persistBuildWrapParams(p, &wrap, &algo)) {
        return false;
    }

    const uint64_t startTime = persistGetMicroseconds();

    size_t payloadLen = 0;
    uint8_t *payload = p->ops->snapshot(structure, &payloadLen);
    if (!payload) {
        return false;
    }

    const size_t envelopeLen =
        flexEnvelopeWrappedSize((uint64_t)payloadLen, algo, wrap.on_wire_flags);
    if (envelopeLen == 0) {
        zfree(payload);
        return false;
    }

    persistStore *store = p->snapshotStore;

    /* A snapshot fully replaces the file, so publish it atomically when
     * the backend supports it: writes land in a private temp that is
     * durably renamed over the live file only once complete. A crash
     * therefore leaves either the prior snapshot or the new one, never
     * a torn mix - and combined with the walSequence watermark, a crash
     * anywhere in compaction stays crash-consistent. */
    const bool atomic =
        store->rewriteBegin && store->rewriteCommit && store->rewriteAbort;

    persistSnapHeader header = {
        .magic = PERSIST_SNAP_MAGIC,
        .version = PERSIST_VERSION,
        .flags = (uint16_t)0,
        .structType = (uint32_t)p->ops->type,
        .count =
            p->ops->count ? (uint64_t)p->ops->count(structure) : (uint64_t)0,
        .envelopeLen = (uint64_t)envelopeLen,
        /* Snapshot reflects every op logged so far, i.e. all sequences
         * below the next-to-assign one. Recorded so replay can skip
         * WAL entries already folded in. */
        .walSequence = p->walSequence,
        /* If a keyring is attached we stamp its key_id; otherwise 0. A
         * verifier with a keyring attached compares this to the keyring's
         * key_id and rejects with PERSIST_ERR_KEY_ID_MISMATCH on a
         * mismatch (operator pointed reader at the wrong key). */
        .keyId = (p->attachedKeyring != NULL) ? (uint32_t)p->attachedKeyId
                                              : (uint32_t)0u,
        .headerXxh64 = 0,
    };
    header.headerXxh64 = persistSnapHeaderDigest(&header);

    /* Braced so the envelope buffer - which is the whole serialized
     * structure - is released before the durability barrier below. */
    {
        cleanerName(zfree, Bytes) uint8_t *envelope = zmalloc(envelopeLen);
        size_t envelopeWritten = 0;
        if (!flexEnvelopeWrap(envelope, envelopeLen, payload,
                              (uint64_t)payloadLen, &wrap, &envelopeWritten) ||
            envelopeWritten != envelopeLen) {
            zfree(payload);
            return false;
        }
        zfree(payload);

        if (atomic && !store->rewriteBegin(store->ctx)) {
            return false;
        }

        if (store->seek(store->ctx, 0, SEEK_SET) < 0) {
            if (atomic) {
                store->rewriteAbort(store->ctx);
            }
            return false;
        }

        if (store->write(store->ctx, &header, sizeof(header)) !=
            (ssize_t)sizeof(header)) {
            if (atomic) {
                store->rewriteAbort(store->ctx);
            }
            return false;
        }

        if (store->write(store->ctx, envelope, envelopeLen) !=
            (ssize_t)envelopeLen) {
            if (atomic) {
                store->rewriteAbort(store->ctx);
            }
            return false;
        }
    }

    if (atomic) {
        /* Commit fsyncs the temp, renames it over the live file, and
         * fsyncs the directory - the whole durability barrier. */
        if (!store->rewriteCommit(store->ctx)) {
            store->rewriteAbort(store->ctx);
            return false;
        }
    } else {
        if (store->truncate) {
            store->truncate(store->ctx);
        }
        if (store->sync) {
            store->sync(store->ctx);
        }
    }

    p->stats.snapshotCount++;
    p->stats.snapshotBytes += sizeof(header) + envelopeLen;
    p->stats.lastSnapshotTime = startTime;
    p->stats.lastSnapshotDuration = persistGetMicroseconds() - startTime;

    return true;
}

/* Read and verify a snapshot header from `store`. On success, *out is
 * populated. Returns true iff the magic, version, and self-check digest
 * all pass and structType matches `expectedType`. */
static bool persistSnapHeaderReadVerify(persistStore *store,
                                        persistType expectedType,
                                        persistSnapHeader *out) {
    persistSnapHeader header;
    if (store->read(store->ctx, &header, sizeof(header)) !=
        (ssize_t)sizeof(header)) {
        return false;
    }
    if (header.magic != PERSIST_SNAP_MAGIC) {
        return false;
    }
    if (header.version != PERSIST_VERSION) {
        return false;
    }
    if (header.flags != 0) {
        return false;
    }
    const uint64_t savedDigest = header.headerXxh64;
    header.headerXxh64 = 0;
    if (persistSnapHeaderDigest(&header) != savedDigest) {
        return false;
    }
    header.headerXxh64 = savedDigest;
    if (header.structType != (uint32_t)expectedType) {
        return false;
    }
    *out = header;
    return true;
}

void *persistRestore(persist *p) {
    if (!p || !p->snapshotStore || !p->ops->restore) {
        return NULL;
    }

    persistStore *store = p->snapshotStore;
    if (store->seek(store->ctx, 0, SEEK_SET) < 0) {
        return NULL;
    }

    persistSnapHeader header;
    if (!persistSnapHeaderReadVerify(store, p->ops->type, &header)) {
        return NULL;
    }
    /* Cross-key safety: identical contract to persistVerifySnapshot. */
    if (!persistCheckHeaderKeyId(p, header.keyId)) {
        return NULL;
    }

    /* Remember what this snapshot already includes so a subsequent WAL
     * replay skips ops folded into it (crash-consistent compaction). */
    p->snapshotWalSequence = header.walSequence;
    if (header.envelopeLen == 0 ||
        header.envelopeLen >
            FLEX_ENVELOPE_DEFAULT_MAX_PAYLOAD + (uint64_t)(64 * 1024)) {
        return NULL;
    }

    /* Braced so the envelope buffer is released as soon as the structure
     * is materialized from it. */
    void *structure = NULL;
    {
        cleanerName(zfree, Bytes) uint8_t *envelope =
            zmalloc((size_t)header.envelopeLen);
        if (store->read(store->ctx, envelope, (size_t)header.envelopeLen) !=
            (ssize_t)header.envelopeLen) {
            return NULL;
        }

        flexEnvelopeVerifyParams verify;
        if (!persistBuildVerifyParams(p, &verify)) {
            return NULL;
        }

        const uint8_t *payload = NULL;
        uint64_t payloadLen = 0;
        if (!flexEnvelopeVerify(envelope, (size_t)header.envelopeLen, &verify,
                                &payload, &payloadLen)) {
            return NULL;
        }

        /* The envelope's payload sits at an arbitrary offset inside the
         * malloc'd envelope buffer (after the varint framing). ops->restore
         * implementations must read multi-byte fields with memcpy rather than
         * casting the pointer — they do not get an aligned buffer. */
        structure = p->ops->restore(payload, (size_t)payloadLen);
    }

    if (structure && p->ops->validate && !p->ops->validate(structure)) {
        if (p->ops->free) {
            p->ops->free(structure);
        }
        return NULL;
    }

    return structure;
}

/* ============================================================================
 * WAL Operations
 * ============================================================================
 */

/* WAL header layout (28 bytes, packed):
 *
 * 0-3: magic (PERSIST_WAL_MAGIC, "DKWL")
 * 4-5: version (PERSIST_VERSION = 2)
 * 6-7: flags (reserved, must be 0)
 * 8-11: structType (must match snapshot's structType)
 * 12-19: startSequence (sequence number of the first entry in this file)
 * 20-23: keyId (attached keyring key_id; 0 = none)
 * 24-27: headerXxh32 (XXH32 over bytes 0..23 — fast bootstrap check) */
typedef struct __attribute__((packed)) persistWALHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t structType;
    uint64_t startSequence;
    uint32_t keyId;
    uint32_t headerXxh32;
} persistWALHeader;

DK_SIZECHECK(persistWALHeader, PERSIST_WAL_HEADER_SIZE);

/* Write a fresh WAL header at offset 0. Caller owns seeking afterward. */
static bool persistWALWriteHeader(persist *p) {
    persistStore *store = p->walStore;
    persistWALHeader header = {
        .magic = PERSIST_WAL_MAGIC,
        .version = PERSIST_VERSION,
        .flags = (uint16_t)0,
        .structType = (uint32_t)p->ops->type,
        .startSequence = p->walSequence,
        .keyId = (p->attachedKeyring != NULL) ? (uint32_t)p->attachedKeyId
                                              : (uint32_t)0u,
        .headerXxh32 = 0,
    };
    header.headerXxh32 = persistWalHeaderDigest(&header);
    if (store->seek(store->ctx, 0, SEEK_SET) < 0) {
        return false;
    }
    if (store->write(store->ctx, &header, sizeof(header)) !=
        (ssize_t)sizeof(header)) {
        return false;
    }
    if (store->sync) {
        store->sync(store->ctx);
    }
    return true;
}

/* Read+verify the WAL header that already exists in `store`. Returns
 * true and leaves the file pointer positioned just after the header iff
 * everything checks out (magic, version, flags, structType, digest).
 * Caller-supplied `expectedType` is what the persist ops define.
 * If `outHeader` is non-NULL, the parsed header (with digest restored)
 * is written there on success. */
static bool persistWALReadVerifyHeader(persistStore *store,
                                       persistType expectedType,
                                       persistWALHeader *outHeader) {
    persistWALHeader header;
    if (store->seek(store->ctx, 0, SEEK_SET) < 0) {
        return false;
    }
    if (store->read(store->ctx, &header, sizeof(header)) !=
        (ssize_t)sizeof(header)) {
        return false;
    }
    if (header.magic != PERSIST_WAL_MAGIC) {
        return false;
    }
    if (header.version != PERSIST_VERSION) {
        return false;
    }
    if (header.flags != 0) {
        return false;
    }
    if (header.structType != (uint32_t)expectedType) {
        return false;
    }
    const uint32_t savedDigest = header.headerXxh32;
    header.headerXxh32 = 0;
    if (persistWalHeaderDigest(&header) != savedDigest) {
        return false;
    }
    header.headerXxh32 = savedDigest;
    if (outHeader != NULL) {
        *outHeader = header;
    }
    return true;
}

/* Initialize WAL file with header — appending if one already exists. */
static bool persistWALInit(persist *p) {
    if (!p->walStore) {
        return false;
    }

    persistStore *store = p->walStore;

    /* If a valid header already exists, just seek to end for appending. */
    if (persistWALReadVerifyHeader(store, p->ops->type, NULL)) {
        store->seek(store->ctx, 0, SEEK_END);
        return true;
    }

    return persistWALWriteHeader(p);
}

/* Flush WAL buffer to storage */
static bool persistWALFlush(persist *p) {
    if (!p->walStore || p->walBufferUsed == 0) {
        return true;
    }

    persistStore *store = p->walStore;
    ssize_t written = store->write(store->ctx, p->walBuffer, p->walBufferUsed);
    if (written != (ssize_t)p->walBufferUsed) {
        return false;
    }

    p->stats.walBytes += p->walBufferUsed;
    p->walBufferUsed = 0;
    return true;
}

/* Write to WAL (buffered) */
static bool persistWALWrite(persist *p, const void *data, size_t len) {
    if (!p->walBuffer) {
        /* No buffer, write directly */
        if (!p->walStore) {
            return false;
        }
        ssize_t written = p->walStore->write(p->walStore->ctx, data, len);
        if (written == (ssize_t)len) {
            p->stats.walBytes += len;
            return true;
        }
        return false;
    }

    const uint8_t *src = data;
    size_t remaining = len;

    while (remaining > 0) {
        size_t space = p->config.walBufferSize - p->walBufferUsed;
        size_t toWrite = remaining < space ? remaining : space;

        memcpy(p->walBuffer + p->walBufferUsed, src, toWrite);
        p->walBufferUsed += toWrite;
        src += toWrite;
        remaining -= toWrite;

        /* Flush if buffer is full */
        if (p->walBufferUsed >= p->config.walBufferSize) {
            if (!persistWALFlush(p)) {
                return false;
            }
        }
    }

    return true;
}

/* Each WAL entry on disk is:
 *
 * envelopeLen (4 bytes, little-endian uint32)
 * envelope (envelopeLen bytes, flexEnvelopeWrap output)
 *
 * The envelope's payload is the 9-byte preamble (seq||op) concatenated
 * with the persistOps.encodeOp body. The envelope carries its own magic,
 * length, algorithm id, and digest, so there is no separate trailing
 * checksum. */
#define PERSIST_WAL_ENTRY_PREAMBLE_LEN ((size_t)9) /* seq(8) + op(1) */

bool persistLogOp(persist *p, persistOp op, const void *args, size_t argc) {
    if (!p || !p->walStore || !p->ops->encodeOp) {
        return false;
    }

    flexEnvelopeWrapParams wrap;
    flexEnvelopeAlgo algo;
    if (!persistBuildWrapParams(p, &wrap, &algo)) {
        return false;
    }

    if (!p->walInitialized) {
        if (!persistWALInit(p)) {
            return false;
        }
        p->walInitialized = true;
    }

    size_t opDataLen = 0;
    uint8_t *opData = p->ops->encodeOp(op, args, argc, &opDataLen);
    if (!opData && opDataLen > 0) {
        return false;
    }

    const size_t payloadLen = PERSIST_WAL_ENTRY_PREAMBLE_LEN + opDataLen;
    const size_t envelopeLen =
        flexEnvelopeWrappedSize((uint64_t)payloadLen, algo, wrap.on_wire_flags);
    if (envelopeLen == 0 || envelopeLen > (size_t)PERSIST_WAL_ENTRY_MAX_BYTES) {
        zfree(opData);
        return false;
    }

    /* Braced so both scratch buffers - each up to a full WAL entry -
     * are released before the sync barrier below, and the payload copy
     * goes as soon as it is wrapped into the envelope. */
    {
        cleanerName(zfree, Bytes) uint8_t *envelope = zmalloc(envelopeLen);

        {
            cleanerName(zfree, Bytes) uint8_t *payload = zmalloc(payloadLen);
            memcpy(payload, &p->walSequence, sizeof(uint64_t));
            payload[sizeof(uint64_t)] = (uint8_t)op;
            if (opData && opDataLen > 0) {
                memcpy(payload + PERSIST_WAL_ENTRY_PREAMBLE_LEN, opData,
                       opDataLen);
            }
            zfree(opData);

            size_t envelopeWritten = 0;
            if (!flexEnvelopeWrap(envelope, envelopeLen, payload,
                                  (uint64_t)payloadLen, &wrap,
                                  &envelopeWritten) ||
                envelopeWritten != envelopeLen) {
                return false;
            }
        }

        const uint32_t envelopeLen32 = (uint32_t)envelopeLen;
        if (!persistWALWrite(p, &envelopeLen32, sizeof(envelopeLen32))) {
            return false;
        }
        if (!persistWALWrite(p, envelope, envelopeLen)) {
            return false;
        }
    }

    p->walSequence++;
    p->stats.walEntries++;

    /* Handle sync mode */
    switch (p->config.syncMode) {
    case PERSIST_SYNC_ALWAYS:
        persistWALFlush(p);
        if (p->walStore->sync) {
            p->walStore->sync(p->walStore->ctx);
        }
        break;
    case PERSIST_SYNC_EVERYSEC: {
        uint64_t now = persistGetMicroseconds();
        if (now - p->lastSyncTime >= 1000000) { /* 1 second */
            persistWALFlush(p);
            if (p->walStore->sync) {
                p->walStore->sync(p->walStore->ctx);
            }
            p->lastSyncTime = now;
        }
        break;
    }
    case PERSIST_SYNC_NONE:
    default:
        break;
    }

    return true;
}

/* Thread-local state for WAL replay to avoid repeated allocation/free.
 * Used by multilist operations during recovery. */
static _Thread_local mflexState *replayMflexState = NULL;

mflexState *persistGetReplayState(void) {
    return replayMflexState;
}

void persistSetReplayState(mflexState *state) {
    replayMflexState = state;
}

/* Read the next WAL entry envelope from the current position of `store`,
 * unwrap it under `p`'s verify policy, and present the payload's split as
 * (seq, op, opData, opDataLen).
 *
 * Return codes:
 * 1 — entry successfully read and verified. *envelope is malloc'd
 * and OWNED by the caller (must zfree). opData points into it.
 * 0 — clean end-of-file. Nothing allocated.
 * -1 — corrupt or truncated entry. Nothing allocated. In strict
 * recovery the caller must abort; in lenient recovery the caller
 * must NOT continue iterating because there is no length-prefix
 * anchor to recover to — the file is treated as ending here. */
static int persistWALReadOneEntry(persist *p, uint8_t **envelopeOut,
                                  size_t *envelopeLenOut, uint64_t *seqOut,
                                  persistOp *opOut, const uint8_t **opDataOut,
                                  size_t *opDataLenOut) {
    persistStore *store = p->walStore;
    uint32_t envelopeLen32 = 0;

    const ssize_t headerRead =
        store->read(store->ctx, &envelopeLen32, sizeof(envelopeLen32));
    if (headerRead == 0) {
        return 0; /* EOF on clean boundary */
    }
    if (headerRead != (ssize_t)sizeof(envelopeLen32)) {
        return -1;
    }
    if (envelopeLen32 == 0 || envelopeLen32 > PERSIST_WAL_ENTRY_MAX_BYTES) {
        return -1;
    }

    const size_t envelopeLen = (size_t)envelopeLen32;
    cleanerName(zfree, Bytes) uint8_t *envelope = zmalloc(envelopeLen);
    if (store->read(store->ctx, envelope, envelopeLen) !=
        (ssize_t)envelopeLen) {
        return -1;
    }

    flexEnvelopeVerifyParams verify;
    if (!persistBuildVerifyParams(p, &verify)) {
        return -1;
    }

    const uint8_t *payload = NULL;
    uint64_t payloadLen = 0;
    if (!flexEnvelopeVerify(envelope, envelopeLen, &verify, &payload,
                            &payloadLen)) {
        return -1;
    }
    if (payloadLen < (uint64_t)PERSIST_WAL_ENTRY_PREAMBLE_LEN) {
        return -1;
    }

    memcpy(seqOut, payload, sizeof(uint64_t));
    *opOut = (persistOp)payload[sizeof(uint64_t)];
    *opDataOut = payload + PERSIST_WAL_ENTRY_PREAMBLE_LEN;
    *opDataLenOut = (size_t)payloadLen - PERSIST_WAL_ENTRY_PREAMBLE_LEN;

    /* Caller owns the envelope now (opData points into it). */
    *envelopeOut = envelope;
    envelope = NULL;
    *envelopeLenOut = envelopeLen;
    return 1;
}

bool persistReplayWAL(persist *p, void *structure) {
    if (!p || !structure || !p->walStore || !p->ops->applyOp) {
        return false;
    }

    persistStore *store = p->walStore;
    persistWALHeader walHdr;
    if (!persistWALReadVerifyHeader(store, p->ops->type, &walHdr)) {
        return false;
    }
    /* Cross-key safety: same contract as snapshot verify. A WAL keyed
     * under a different id than the currently-attached key would still
     * have produced VERIFY_FAILED on the very first envelope, but the
     * typed early reject lets the caller distinguish "operator pointed
     * me at the wrong key" from generic envelope corruption. */
    if (!persistCheckHeaderKeyId(p, walHdr.keyId)) {
        return false;
    }

    while (1) {
        uint8_t *envelope = NULL;
        size_t envelopeLen = 0;
        uint64_t seq = 0;
        persistOp op = PERSIST_OP_NOP;
        const uint8_t *opData = NULL;
        size_t opDataLen = 0;

        const int rc = persistWALReadOneEntry(p, &envelope, &envelopeLen, &seq,
                                              &op, &opData, &opDataLen);
        if (rc == 0) {
            return true;
        }
        if (rc < 0) {
            p->stats.entriesSkipped++;
            return p->config.strictRecovery ? false : true;
        }

        /* Already folded into the restored snapshot (a stale WAL left by
         * a crash between snapshot publish and WAL truncate): applying
         * it again would double-count. Advance the sequence and move on. */
        if (seq < p->snapshotWalSequence) {
            if (seq >= p->walSequence) {
                p->walSequence = seq + 1;
            }
            p->stats.entriesSkipped++;
            zfree(envelope);
            continue;
        }

        if (!p->ops->applyOp(structure, op, opData, opDataLen)) {
            zfree(envelope);
            p->stats.entriesSkipped++;
            if (p->config.strictRecovery) {
                return false;
            }
            continue;
        }

        if (seq >= p->walSequence) {
            p->walSequence = seq + 1;
        }
        p->stats.entriesRecovered++;
        zfree(envelope);
    }
}

bool persistIterateWAL(persist *p, persistWALCallback cb, void *ctx) {
    if (!p || !p->walStore || !cb) {
        return false;
    }

    persistStore *store = p->walStore;
    if (!persistWALReadVerifyHeader(store, p->ops->type, NULL)) {
        return false;
    }

    while (1) {
        uint8_t *envelope = NULL;
        size_t envelopeLen = 0;
        uint64_t seq = 0;
        persistOp op = PERSIST_OP_NOP;
        const uint8_t *opData = NULL;
        size_t opDataLen = 0;

        const int rc = persistWALReadOneEntry(p, &envelope, &envelopeLen, &seq,
                                              &op, &opData, &opDataLen);
        if (rc <= 0) {
            return rc == 0;
        }

        persistWALEntry walEntry = {
            .sequence = seq,
            .op = op,
            .data = opData,
            .len = opDataLen,
        };
        const bool cont = cb(&walEntry, ctx);
        zfree(envelope);
        if (!cont) {
            return true;
        }
    }
}

/* ============================================================================
 * Compaction and Recovery
 * ============================================================================
 */

bool persistShouldCompact(const persist *p) {
    if (!p || !p->config.autoCompact) {
        return false;
    }

    if (!p->walStore || !p->snapshotStore) {
        return false;
    }

    int64_t walSize = p->walStore->size(p->walStore->ctx);
    if (walSize < 0) {
        return false;
    }

    /* Check absolute WAL size limit */
    if ((size_t)walSize > p->config.walMaxSize) {
        return true;
    }

    /* Check ratio to snapshot */
    int64_t snapSize = p->snapshotStore->size(p->snapshotStore->ctx);
    if (snapSize > 0 &&
        (double)walSize > (double)snapSize * p->config.compactRatio) {
        return true;
    }

    return false;
}

bool persistCompact(persist *p, void *structure) {
    if (!p || !structure) {
        return false;
    }

    if (!persistSnapshot(p, structure)) {
        return false;
    }

    if (p->walStore) {
        persistStore *store = p->walStore;
        if (store->seek(store->ctx, 0, SEEK_SET) >= 0) {
            if (store->truncate) {
                store->truncate(store->ctx);
            }
        }
        p->walBufferUsed = 0;
        if (!persistWALWriteHeader(p)) {
            return false;
        }
    }

    p->stats.compactionCount++;
    p->stats.lastCompactionTime = persistGetMicroseconds();
    return true;
}

void *persistRecover(persist *p) {
    if (!p) {
        return NULL;
    }

    p->stats.recoveryCount++;

    /* Create mflexState for this recovery session if structure uses multilist.
     * Per-tier multilist types (SMALL/MEDIUM/FULL) route through the same
     * apply path (multilistPersistApplyOp via persistGetReplayState), so
     * each tier requires the same thread-local mflexState during WAL replay.
     * Missing any tier here would re-introduce a per-tier replay deref of
     * NULL mflexState → segfault. */
    if (p->ops->type == PERSIST_TYPE_MULTILIST ||
        p->ops->type == PERSIST_TYPE_MULTILIST_SMALL ||
        p->ops->type == PERSIST_TYPE_MULTILIST_MEDIUM ||
        p->ops->type == PERSIST_TYPE_MULTILIST_FULL) {
        /* @own: static */
        replayMflexState = mflexStateCreate();
        if (!replayMflexState) {
            return NULL;
        }
    }

    /* Try to restore from snapshot */
    void *structure = persistRestore(p);
    if (!structure) {
        /* No valid snapshot - structure operations should handle this case */
        if (replayMflexState) {
            mflexStateFree(replayMflexState);
            replayMflexState = NULL;
        }
        return NULL;
    }

    /* Replay WAL if available */
    if (p->walStore) {
        /* Reset recovery stats */
        p->stats.entriesRecovered = 0;
        p->stats.entriesSkipped = 0;

        /* Pass pointer to structure pointer so WAL replay can update it
         * if the structure is reallocated (e.g., flex during pushes) */
        if (!persistReplayWAL(p, &structure)) {
            if (p->config.strictRecovery) {
                if (p->ops->free) {
                    p->ops->free(structure);
                }
                if (replayMflexState) {
                    mflexStateFree(replayMflexState);
                    replayMflexState = NULL;
                }
                return NULL;
            }
            /* Continue with partial recovery in lenient mode */
        }
    }

    /* Clean up replay state */
    if (replayMflexState) {
        mflexStateFree(replayMflexState);
        replayMflexState = NULL;
    }

    return structure;
}

bool persistSync(persist *p) {
    if (!p) {
        return false;
    }

    /* Flush WAL buffer */
    if (!persistWALFlush(p)) {
        return false;
    }

    /* Sync WAL to disk */
    if (p->walStore && p->walStore->sync) {
        if (!p->walStore->sync(p->walStore->ctx)) {
            return false;
        }
    }

    p->lastSyncTime = persistGetMicroseconds();
    return true;
}

/* ============================================================================
 * Verification Utilities
 * ============================================================================
 */

bool persistVerifySnapshot(persist *p) {
    if (!p || !p->snapshotStore || !p->ops) {
        return false;
    }

    persistStore *store = p->snapshotStore;
    if (store->seek(store->ctx, 0, SEEK_SET) < 0) {
        return false;
    }

    persistSnapHeader header;
    if (!persistSnapHeaderReadVerify(store, p->ops->type, &header)) {
        return false;
    }
    /* Cross-key safety: when a keyring is attached, reject early if the
     * header's recorded key_id doesn't match. This turns the opaque
     * VERIFY_FAILED an operator would otherwise get (HMAC mismatch on
     * the body) into the typed PERSIST_ERR_KEY_ID_MISMATCH so the
     * operator can recover via persistLastExpectedKeyId and
     * persistRebindKeyring. */
    if (!persistCheckHeaderKeyId(p, header.keyId)) {
        return false;
    }
    if (header.envelopeLen == 0 ||
        header.envelopeLen >
            FLEX_ENVELOPE_DEFAULT_MAX_PAYLOAD + (uint64_t)(64 * 1024)) {
        return false;
    }

    cleanerName(zfree, Bytes) uint8_t *envelope =
        zmalloc((size_t)header.envelopeLen);
    if (store->read(store->ctx, envelope, (size_t)header.envelopeLen) !=
        (ssize_t)header.envelopeLen) {
        return false;
    }

    flexEnvelopeVerifyParams verify;
    if (!persistBuildVerifyParams(p, &verify)) {
        return false;
    }

    const uint8_t *payload = NULL;
    uint64_t payloadLen = 0;
    return flexEnvelopeVerify(envelope, (size_t)header.envelopeLen, &verify,
                              &payload, &payloadLen);
}

bool persistVerifyWAL(persist *p) {
    if (!p || !p->walStore || !p->ops) {
        return false;
    }

    persistStore *store = p->walStore;
    persistWALHeader walHdr;
    if (!persistWALReadVerifyHeader(store, p->ops->type, &walHdr)) {
        return false;
    }
    /* Cross-key safety: same contract as persistVerifySnapshot. */
    if (!persistCheckHeaderKeyId(p, walHdr.keyId)) {
        return false;
    }

    while (1) {
        uint8_t *envelope = NULL;
        size_t envelopeLen = 0;
        uint64_t seq = 0;
        persistOp op = PERSIST_OP_NOP;
        const uint8_t *opData = NULL;
        size_t opDataLen = 0;

        const int rc = persistWALReadOneEntry(p, &envelope, &envelopeLen, &seq,
                                              &op, &opData, &opDataLen);
        if (rc == 0) {
            return true;
        }
        if (rc < 0) {
            return false;
        }
        zfree(envelope);
    }
}

/* ============================================================================
 * Name Lookup Utilities
 * ============================================================================
 */

const char *persistTypeName(persistType type) {
    switch (type) {
    case PERSIST_TYPE_FLEX:
        return "flex";
    case PERSIST_TYPE_INTSET:
        return "intset";
    case PERSIST_TYPE_INTSETBIG:
        return "intsetBig";
    case PERSIST_TYPE_MULTILIST:
        return "multilist";
    case PERSIST_TYPE_MULTILIST_SMALL:
        return "multilistSmall";
    case PERSIST_TYPE_MULTILIST_MEDIUM:
        return "multilistMedium";
    case PERSIST_TYPE_MULTILIST_FULL:
        return "multilistFull";
    case PERSIST_TYPE_MULTIMAP:
        return "multimap";
    case PERSIST_TYPE_MULTIMAP_SMALL:
        return "multimapSmall";
    case PERSIST_TYPE_MULTIMAP_MEDIUM:
        return "multimapMedium";
    case PERSIST_TYPE_MULTIMAP_FULL:
        return "multimapFull";
    case PERSIST_TYPE_MULTIDICT:
        return "multidict";
    case PERSIST_TYPE_MULTIARRAY:
        return "multiarray";
    case PERSIST_TYPE_MULTIARRAY_SMALL:
        return "multiarraySmall";
    case PERSIST_TYPE_MULTIARRAY_MEDIUM:
        return "multiarrayMedium";
    case PERSIST_TYPE_MULTIARRAY_LARGE:
        return "multiarrayLarge";
    case PERSIST_TYPE_MULTIORDEREDSET:
        return "multiOrderedSet";
    case PERSIST_TYPE_MULTILRU:
        return "multilru";
    case PERSIST_TYPE_MULTIROAR:
        return "multiroar";
    case PERSIST_TYPE_LINEARBLOOM:
        return "linearBloom";
    case PERSIST_TYPE_HYPERLOGLOG:
        return "hyperloglog";
    case PERSIST_TYPE_HMAC_KEYRING:
        return "hmacKeyring";
    case PERSIST_TYPE_CLUSTER_MAILBOX_WATERMARKS:
        return "clusterMailboxWatermarks";
    case PERSIST_TYPE_CLUSTER_RING:
        return "clusterRing";
    case PERSIST_TYPE_AEAD_NONCE_LOG:
        return "aeadNonceLog";
    default:
        return "unknown";
    }
}

const char *persistOpName(persistOp op) {
    switch (op) {
    case PERSIST_OP_NOP:
        return "NOP";
    case PERSIST_OP_INSERT:
        return "INSERT";
    case PERSIST_OP_DELETE:
        return "DELETE";
    case PERSIST_OP_UPDATE:
        return "UPDATE";
    case PERSIST_OP_REPLACE:
        return "REPLACE";
    case PERSIST_OP_PUSH_HEAD:
        return "PUSH_HEAD";
    case PERSIST_OP_PUSH_TAIL:
        return "PUSH_TAIL";
    case PERSIST_OP_POP_HEAD:
        return "POP_HEAD";
    case PERSIST_OP_POP_TAIL:
        return "POP_TAIL";
    case PERSIST_OP_INSERT_AT:
        return "INSERT_AT";
    case PERSIST_OP_DELETE_AT:
        return "DELETE_AT";
    case PERSIST_OP_CLEAR:
        return "CLEAR";
    case PERSIST_OP_BULK_INSERT:
        return "BULK_INSERT";
    case PERSIST_OP_BULK_DELETE:
        return "BULK_DELETE";
    case PERSIST_OP_MERGE:
        return "MERGE";
    case PERSIST_OP_CUSTOM:
        return "CUSTOM";
    default:
        return "UNKNOWN";
    }
}

/* ============================================================================
 * Memory Storage Backend
 * ============================================================================
 */
typedef struct persistMemStore {
    uint8_t *data;
    size_t size;
    size_t capacity;
    size_t position;
} persistMemStore;

static ssize_t persistMemWrite(void *ctx, const void *data, size_t len) {
    persistMemStore *m = ctx;

    /* Grow if needed */
    size_t needed = m->position + len;
    if (needed > m->capacity) {
        size_t newCap = m->capacity ? m->capacity * 2 : 4096;
        while (newCap < needed) {
            newCap *= 2;
        }
        /* @own: container; free=persistMemClose */
        m->data = zrealloc(m->data, newCap);
        m->capacity = newCap;
    }

    memcpy(m->data + m->position, data, len);
    m->position += len;
    if (m->position > m->size) {
        m->size = m->position;
    }

    return (ssize_t)len;
}

static ssize_t persistMemRead(void *ctx, void *buf, size_t len) {
    persistMemStore *m = ctx;

    size_t available = m->size - m->position;
    size_t toRead = len < available ? len : available;

    if (toRead > 0) {
        memcpy(buf, m->data + m->position, toRead);
        m->position += toRead;
    }

    return (ssize_t)toRead;
}

static int64_t persistMemSeek(void *ctx, int64_t offset, int whence) {
    persistMemStore *m = ctx;

    int64_t newPos;
    switch (whence) {
    case SEEK_SET:
        newPos = offset;
        break;
    case SEEK_CUR:
        newPos = (int64_t)m->position + offset;
        break;
    case SEEK_END:
        newPos = (int64_t)m->size + offset;
        break;
    default:
        return -1;
    }

    if (newPos < 0) {
        return -1;
    }

    m->position = (size_t)newPos;
    return (int64_t)m->position;
}

static int64_t persistMemTell(void *ctx) {
    persistMemStore *m = ctx;
    return (int64_t)m->position;
}

static bool persistMemSync(void *ctx) {
    (void)ctx;
    return true; /* Memory is always "synced" */
}

static bool persistMemTruncate(void *ctx) {
    persistMemStore *m = ctx;
    m->size = m->position;
    return true;
}

static int64_t persistMemSize(void *ctx) {
    persistMemStore *m = ctx;
    return (int64_t)m->size;
}

static void persistMemClose(void *ctx) {
    persistMemStore *m = ctx;
    zfree(m->data);
    zfree(m);
}

persistStore *persistStoreMemory(size_t initialCapacity) {
    /* @own: container; free=persistMemClose */
    persistMemStore *m = zmalloc(sizeof(*m));
    /* @own: container; free=persistMemClose */
    m->data = initialCapacity > 0 ? zmalloc(initialCapacity) : NULL;
    m->size = 0;
    m->capacity = initialCapacity;
    m->position = 0;

    /* @own: caller; free=zfree */
    persistStore *store = zmalloc(sizeof(*store));
    store->ctx = m;
    store->write = persistMemWrite;
    store->read = persistMemRead;
    store->seek = persistMemSeek;
    store->tell = persistMemTell;
    store->sync = persistMemSync;
    store->truncate = persistMemTruncate;
    store->size = persistMemSize;
    store->close = persistMemClose;
    /* Memory has no crash exposure: in-place overwrite is fine. */
    store->rewriteBegin = NULL;
    store->rewriteCommit = NULL;
    store->rewriteAbort = NULL;

    return store;
}

const uint8_t *persistStoreMemoryBuffer(persistStore *store, size_t *len) {
    if (!store || !store->ctx) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }

    persistMemStore *m = store->ctx;
    if (len) {
        *len = m->size;
    }
    return m->data;
}

/* ============================================================================
 * File Storage Backend
 * ============================================================================
 */
typedef struct persistFileStore {
    int fd; /* live/committed file */
    char *path;
    int tmpFd;     /* temp file during an atomic rewrite, else -1 */
    char *tmpPath; /* its path during a rewrite, else NULL */
} persistFileStore;

/* Writes/seeks/truncates target the temp file mid-rewrite, the live
 * file otherwise. Reads always come from the live file. */
static int persistFileActiveFd(const persistFileStore *f) {
    return f->tmpFd >= 0 ? f->tmpFd : f->fd;
}

static ssize_t persistFileWrite(void *ctx, const void *data, size_t len) {
    persistFileStore *f = ctx;
    return write(persistFileActiveFd(f), data, len);
}

static ssize_t persistFileRead(void *ctx, void *buf, size_t len) {
    persistFileStore *f = ctx;
    return read(f->fd, buf, len);
}

static int64_t persistFileSeek(void *ctx, int64_t offset, int whence) {
    persistFileStore *f = ctx;
    return lseek(persistFileActiveFd(f), offset, whence);
}

static int64_t persistFileTell(void *ctx) {
    persistFileStore *f = ctx;
    return lseek(persistFileActiveFd(f), 0, SEEK_CUR);
}

static bool persistFileSync(void *ctx) {
    persistFileStore *f = ctx;
    return dk_fsync(persistFileActiveFd(f)) == 0;
}

static bool persistFileTruncate(void *ctx) {
    persistFileStore *f = ctx;
    const int fd = persistFileActiveFd(f);
    off_t pos = lseek(fd, 0, SEEK_CUR);
    if (pos < 0) {
        return false;
    }
    return ftruncate(fd, pos) == 0;
}

static int64_t persistFileSize(void *ctx) {
    persistFileStore *f = ctx;
    struct stat st;
    if (fstat(persistFileActiveFd(f), &st) < 0) {
        return -1;
    }
    return st.st_size;
}

/* fsync the directory holding `path` so a rename into it is itself
 * durable (the rename's metadata otherwise lives only in the dir
 * inode's dirty page). */
static bool persistFsyncDir(const char *path) {
    const char *slash = strrchr(path, '/');
    char dirbuf[2] = ".";
    const char *dir;
    cleanerName(zfree, Chars) char *owned = NULL;
    if (!slash) {
        dir = dirbuf;
    } else if (slash == path) {
        dirbuf[0] = '/';
        dir = dirbuf;
    } else {
        const size_t n = (size_t)(slash - path);
        owned = zmalloc(n + 1);
        memcpy(owned, path, n);
        owned[n] = '\0';
        dir = owned;
    }

    const int dfd = open(dir, O_RDONLY);
    if (dfd < 0) {
        return false;
    }

    const bool ok = dk_fsync(dfd) == 0;
    close(dfd);
    return ok;
}

static bool persistFileRewriteBegin(void *ctx) {
    persistFileStore *f = ctx;
    if (f->tmpFd >= 0) {
        return false; /* a rewrite is already in progress */
    }

    const size_t pathLen = strlen(f->path);
    cleanerName(zfree, Chars) char *tmpPath = zmalloc(pathLen + sizeof(".tmp"));
    memcpy(tmpPath, f->path, pathLen);
    memcpy(tmpPath + pathLen, ".tmp", sizeof(".tmp"));

    const int tmpFd = open(tmpPath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (tmpFd < 0) {
        return false;
    }

    f->tmpFd = tmpFd;
    /* The store owns the temp path until the rewrite ends. */
    f->tmpPath = tmpPath;
    tmpPath = NULL;
    return true;
}

static bool persistFileRewriteCommit(void *ctx) {
    persistFileStore *f = ctx;
    if (f->tmpFd < 0) {
        return false;
    }

    /* Durably publish: the temp's bytes must hit disk before the
     * rename, and the rename before we report success. */
    if (dk_fsync(f->tmpFd) != 0) {
        return false;
    }
    if (rename(f->tmpPath, f->path) != 0) {
        return false;
    }
    if (!persistFsyncDir(f->path)) {
        return false;
    }

    /* The temp fd now refers to the live path; retire the old fd. */
    close(f->fd);
    f->fd = f->tmpFd;
    f->tmpFd = -1;
    zfree(f->tmpPath);
    f->tmpPath = NULL;
    return true;
}

static void persistFileRewriteAbort(void *ctx) {
    persistFileStore *f = ctx;
    if (f->tmpFd < 0) {
        return;
    }

    close(f->tmpFd);
    unlink(f->tmpPath);
    f->tmpFd = -1;
    zfree(f->tmpPath);
    f->tmpPath = NULL;
}

static void persistFileClose(void *ctx) {
    persistFileStore *f = ctx;
    if (f->tmpFd >= 0) {
        /* An unfinished rewrite: discard the temp, keep the live file. */
        close(f->tmpFd);
        unlink(f->tmpPath);
        zfree(f->tmpPath);
    }
    close(f->fd);
    zfree(f->path);
    zfree(f);
}

persistStore *persistStoreFile(const char *path, bool create) {
    if (!path) {
        return NULL;
    }

    int flags = O_RDWR;
    if (create) {
        flags |= O_CREAT;
    }

    int fd = open(path, flags, 0644);
    if (fd < 0) {
        return NULL;
    }

    /* @own: container; free=persistFileClose */
    persistFileStore *f = zmalloc(sizeof(*f));
    f->fd = fd;
    /* @own: container; free=persistFileClose */
    f->path = zmalloc(strlen(path) + 1);
    strcpy(f->path, path);
    f->tmpFd = -1;
    f->tmpPath = NULL;

    /* @own: caller; free=zfree */
    persistStore *store = zmalloc(sizeof(*store));
    store->ctx = f;
    store->write = persistFileWrite;
    store->read = persistFileRead;
    store->seek = persistFileSeek;
    store->tell = persistFileTell;
    store->sync = persistFileSync;
    store->truncate = persistFileTruncate;
    store->size = persistFileSize;
    store->close = persistFileClose;
    store->rewriteBegin = persistFileRewriteBegin;
    store->rewriteCommit = persistFileRewriteCommit;
    store->rewriteAbort = persistFileRewriteAbort;

    return store;
}

/* ============================================================================
 * Structure-Specific Operations: flex
 * ============================================================================
 */
#include "flex.h"

static uint8_t *flexPersistSnapshot(const void *structure, size_t *len) {
    const flex *f = structure;
    *len = flexBytes(f);
    /* @own: caller; free=zfree */
    uint8_t *buf = zmalloc(*len);
    memcpy(buf, f, *len);
    return buf;
}

static void *flexPersistRestore(const uint8_t *data, size_t len) {
    /* The restored flex is iterated by its owner, so every entry is
     * checked, not just the header.  flexValidateBufferDeep also refuses
     * NULL; testing it here as well lets the compiler see that the copy
     * below never reads from NULL. */
    if (!data || !flexValidateBufferDeep(data, len)) {
        return NULL;
    }
    /* @own: caller; free=flexFree */
    flex *f = zmalloc(len);
    memcpy(f, data, len);
    return f;
}

static size_t flexPersistCount(const void *structure) {
    return flexCount(structure);
}

static size_t flexPersistEstimateSize(const void *structure) {
    return flexBytes(structure);
}

static uint8_t *flexPersistEncodeOp(persistOp op, const void *args, size_t argc,
                                    size_t *len) {
    /* args is one const databox* */
    const databox *box = args;
    *len = 0;

    switch (op) {
    case PERSIST_OP_PUSH_HEAD:
    case PERSIST_OP_PUSH_TAIL: {
        if (!box || argc < 1) {
            return NULL;
        }

        /* The value as a 1-element flex, stored exactly as given, so
         * 128-bit values and numeral-looking strings replay unchanged */
        flex *f = flexNewFromDataboxExact(box);
        if (f) {
            *len = flexBytes(f);
        }
        return (uint8_t *)f;
    }

    default:
        /* CLEAR has no operand */
        return NULL;
    }
}

static bool flexPersistApplyOp(void *structure, persistOp op,
                               const uint8_t *data, size_t len) {
    flex **ff = structure; /* We expect a pointer to the flex pointer */

    switch (op) {
    case PERSIST_OP_PUSH_HEAD:
    case PERSIST_OP_PUSH_TAIL: {
        databoxBig box;
        if (!flexGetSingleDataboxBig(data, len, &box)) {
            return false;
        }

        flexPushByTypeExact(ff, (const databox *)&box,
                            op == PERSIST_OP_PUSH_HEAD ? FLEX_ENDPOINT_HEAD
                                                       : FLEX_ENDPOINT_TAIL);
        return true;
    }

    case PERSIST_OP_CLEAR:
        flexReset(ff);
        return true;

    default:
        return false;
    }
}

static void flexPersistFree(void *structure) {
    flexFree(structure);
}

static void *flexPersistNewEmpty(void) {
    /* @own: caller; free=flexFree */
    return flexNew();
}

static bool flexPersistValidate(const void *structure) {
    const flex *f = structure;
    return flexValidateBufferDeep((const uint8_t *)f, flexBytes(f));
}

const persistOps persistOpsFlex = {
    .type = PERSIST_TYPE_FLEX,
    .name = "flex",
    .snapshot = flexPersistSnapshot,
    .restore = flexPersistRestore,
    .newEmpty = flexPersistNewEmpty,
    .count = flexPersistCount,
    .estimateSize = flexPersistEstimateSize,
    .encodeOp = flexPersistEncodeOp,
    .applyOp = flexPersistApplyOp,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = flexPersistValidate,
    .free = flexPersistFree,
};

/* ============================================================================
 * Structure-Specific Operations: intset
 * ============================================================================
 */
#include "intset.h"

defcleaner(intset *, intsetFree);

static uint8_t *intsetPersistSnapshot(const void *structure, size_t *len) {
    intset *is = (intset *)structure;
    size_t count = intsetCount(is);

    /* Snapshot format: [count: 8 bytes][values: count * 8 bytes]
     * We serialize all values as int64_t for universality */
    *len = sizeof(uint64_t) + (count * sizeof(int64_t));
    /* @own: caller; free=zfree */
    uint8_t *buf = zmalloc(*len);

    /* Write count */
    uint64_t countU64 = count;
    memcpy(buf, &countU64, sizeof(uint64_t));

    /* Every value in ascending order, O(1) each */
    intsetIterator it;
    intsetIteratorInit(&it, is);
    uint8_t *at = buf + sizeof(uint64_t);
    int64_t value;
    while (intsetIteratorNext(&it, &value)) {
        memcpy(at, &value, sizeof(value));
        at += sizeof(value);
    }

    return buf;
}

static void *intsetPersistRestore(const uint8_t *data, size_t len) {
    /* Minimum valid length: count field */
    if (!data || len < sizeof(uint64_t)) {
        return NULL;
    }

    /* Read count */
    uint64_t count;
    memcpy(&count, data, sizeof(uint64_t));

    /* Refuse counts when could overflow size_t when
     * multiplied by sizeof(int64_t). Without this, a hostile or
     * corrupted input could pass len-check via integer wrap then
     * cause out-of-bounds reads inside the values loop. */
    if (count > (SIZE_MAX - sizeof(uint64_t)) / sizeof(int64_t)) {
        return NULL;
    }

    /* Exactly the values the count names */
    size_t expectedLen = sizeof(uint64_t) + (count * sizeof(int64_t));
    if (len != expectedLen) {
        return NULL;
    }

    /* The values as the snapshot wrote them: strictly ascending (damaged
     * bytes are refused, not re-sorted), so every add appends to the end
     * of its width's array.  memcpy rather than a pointer cast: the
     * payload may sit at any offset inside the envelope, so an
     * int64_t-aligned address is not guaranteed. */
    cleaner(intsetFree) intset *is = intsetNew();
    const uint8_t *valueBytes = data + sizeof(uint64_t);
    int64_t prev = 0;
    for (uint64_t i = 0; i < count; i++) {
        int64_t value;
        memcpy(&value, valueBytes + i * sizeof(int64_t), sizeof(int64_t));
        if (i > 0 && value <= prev) {
            return NULL;
        }
        prev = value;
        bool success;
        intsetAdd(&is, value, &success);
    }

    /* @own: caller; free=intsetFree */
    intset *out = is;
    is = NULL;
    return out;
}

static size_t intsetPersistCount(const void *structure) {
    return intsetCount(structure);
}

static size_t intsetPersistEstimateSize(const void *structure) {
    return intsetBytes((intset *)structure);
}

static uint8_t *intsetPersistEncodeOp(persistOp op, const void *args,
                                      size_t argc, size_t *len) {
    (void)argc;

    switch (op) {
    case PERSIST_OP_INSERT:
    case PERSIST_OP_DELETE: {
        /* args is int64_t* */
        const int64_t *value = args;
        *len = sizeof(int64_t);
        /* @own: caller; free=zfree */
        uint8_t *buf = zmalloc(*len);
        memcpy(buf, value, sizeof(int64_t));
        return buf;
    }

    case PERSIST_OP_CLEAR:
        *len = 0;
        return NULL;

    default:
        *len = 0;
        return NULL;
    }
}

static bool intsetPersistApplyOp(void *structure, persistOp op,
                                 const uint8_t *data, size_t len) {
    intset **is = structure; /* Pointer to intset pointer */

    switch (op) {
    case PERSIST_OP_INSERT: {
        if (len != sizeof(int64_t)) {
            return false;
        }
        int64_t value;
        memcpy(&value, data, sizeof(int64_t));
        bool success;
        intsetAdd(is, value, &success);
        return true; /* Return true even if already exists */
    }

    case PERSIST_OP_DELETE: {
        if (len != sizeof(int64_t)) {
            return false;
        }
        int64_t value;
        memcpy(&value, data, sizeof(int64_t));
        bool success;
        intsetRemove(is, value, &success);
        return true;
    }

    case PERSIST_OP_CLEAR: {
        intsetFree(*is);
        /* @own: caller; free=intsetFree */
        *is = intsetNew();
        return true;
    }

    default:
        return false;
    }
}

static void intsetPersistFree(void *structure) {
    intsetFree(structure);
}

static void *intsetPersistNewEmpty(void) {
    /* @own: caller; free=intsetFree */
    return intsetNew();
}

const persistOps persistOpsIntset = {
    .type = PERSIST_TYPE_INTSET,
    .name = "intset",
    .snapshot = intsetPersistSnapshot,
    .restore = intsetPersistRestore,
    .newEmpty = intsetPersistNewEmpty,
    .count = intsetPersistCount,
    .estimateSize = intsetPersistEstimateSize,
    .encodeOp = intsetPersistEncodeOp,
    .applyOp = intsetPersistApplyOp,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = NULL,
    .free = intsetPersistFree,
};

/* ============================================================================
 * Structure-Specific Operations: intsetBig
 * ============================================================================
 *
 * Snapshot: a flex holding every member (signed/unsigned, 64 or 128-bit),
 * stored exactly.  WAL: INSERT and DELETE carry one member as a 1-element
 * flex; CLEAR has no operand.
 *
 * intsetBig stores numeric values directly with no atom-table
 * indirection — single byte content represents the same value on every
 * serialized output, so no multi-data-structure binding is needed for
 * set details or compares or auditing of different representations here.
 */
#include "intsetBig.h"

defcleaner(intsetBig *, intsetBigFree);

static uint8_t *intsetBigPersistSnapshot(const void *structure, size_t *len) {
    const intsetBig *isb = structure;
    /* A flex whose elements are the numeric members (one per member), built
     * incrementally via the databoxBig factory push — no count header, no
     * hand-rolled [tag][value] framing. */
    /* The flex buffer itself is the snapshot payload; the framework
     * releases snapshot buffers with zfree, which is what flexFree does. */
    /* @own: caller; free=zfree */
    flex *f = flexNew();
    if (!f) {
        *len = 0;
        return NULL;
    }
    if (isb) {
        /* Members are appended a chunk at a time, each decoded into its
         * own slot (a 96/128-bit value lives in its databoxBig), so a
         * large set grows its flex once per chunk, not once per member */
        enum { ISB_CHUNK_ = 32 };
        databoxBig values[ISB_CHUNK_];
        const databox *chunk[ISB_CHUNK_];
        intsetBigIterator it = {0};
        intsetBigIteratorInit(isb, &it);
        uint32_t n = 0;
        memset(&values[0], 0, sizeof(values[0]));
        while (intsetBigIteratorNextBox(&it, &values[n])) {
            chunk[n] = (const databox *)&values[n];
            if (++n == ISB_CHUNK_) {
                flexAppendMultipleExact(&f, n, chunk);
                n = 0;
            }

            memset(&values[n], 0, sizeof(values[n]));
        }

        if (n) {
            flexAppendMultipleExact(&f, n, chunk);
        }
    }
    *len = flexBytes(f);
    return (uint8_t *)f;
}

static void *intsetBigPersistRestore(const uint8_t *data, size_t len) {
    /* Defensive: refuse a malformed flex from replication / on-disk recovery
     * rather than walk past its header. */
    if (!flexValidateBufferDeep(data, len)) {
        return NULL;
    }

    /* The members in the order the snapshot walked them: built a bucket at
     * a time; members out of that order or out of range are refused */
    /* @own: caller; free=intsetBigFree */
    return intsetBigNewFromMembers((const flex *)data);
}

static size_t intsetBigPersistCount(const void *structure) {
    return intsetBigCountElements(structure);
}

static size_t intsetBigPersistEstimateSize(const void *structure) {
    return intsetBigBytes((intsetBig *)structure);
}

/* applyOp: PERSIST_OP_INSERT adds, PERSIST_OP_DELETE removes a single
 * value encoded with the same single-entry shape; PERSIST_OP_CLEAR empties
 * the set. */
static bool intsetBigPersistApplyOp(void *structure, persistOp op,
                                    const uint8_t *data, size_t len) {
    intsetBig **isb_p = structure;
    if (op == PERSIST_OP_CLEAR) {
        intsetBigFree(*isb_p);
        /* @own: caller; free=intsetBigFree */
        *isb_p = intsetBigNew();
        return true;
    }

    intsetBig *isb = *isb_p;
    /* Unwrap the 1-element flex back to the numeric member (128-bit-capable;
     * defensive against malformed replicated bytes). */
    databoxBig dbig;
    if (!flexGetSingleDataboxBig(data, len, &dbig) ||
        !intsetBigCanHold(&dbig)) {
        return false;
    }
    switch (op) {
    case PERSIST_OP_INSERT:
        intsetBigAdd(isb, &dbig);
        break;
    case PERSIST_OP_DELETE:
        intsetBigRemove(isb, &dbig);
        break;
    default:
        return false;
    }
    return true;
}

static uint8_t *intsetBigPersistEncodeOp(persistOp op, const void *args,
                                         size_t argc, size_t *len) {
    (void)argc;
    *len = 0;
    if (op != PERSIST_OP_INSERT && op != PERSIST_OP_DELETE) {
        /* CLEAR has no operand; nothing else is replayed */
        return NULL;
    }

    /* args is a const databoxBig *.  Only numeric members belong in an
     * intsetBig; reject anything else (the factory would otherwise push it
     * verbatim).  The member is written as a 1-element flex through the
     * databoxBig factory arm, like any other value. */
    const databoxBig *b = args;
    if (!b || !intsetBigCanHold(b)) {
        return NULL;
    }
    flex *f = flexNewFromDataboxBigExact(b);
    if (!f) {
        *len = 0;
        return NULL;
    }
    *len = flexBytes(f);
    return (uint8_t *)f;
}

static void *intsetBigPersistNewEmpty(void) {
    /* @own: caller; free=intsetBigFree */
    return intsetBigNew();
}

static void intsetBigPersistFree(void *structure) {
    intsetBigFree(structure);
}

const persistOps persistOpsIntsetBig = {
    .deletesWhenEmpty = true,
    .type = PERSIST_TYPE_INTSETBIG,
    .name = "intsetBig",
    .snapshot = intsetBigPersistSnapshot,
    .restore = intsetBigPersistRestore,
    .newEmpty = intsetBigPersistNewEmpty,
    .count = intsetBigPersistCount,
    .estimateSize = intsetBigPersistEstimateSize,
    .encodeOp = intsetBigPersistEncodeOp,
    .applyOp = intsetBigPersistApplyOp,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = NULL,
    .free = intsetBigPersistFree,
};

/* ============================================================================
 * Structure-Specific Operations: multiroar
 * ============================================================================
 */
#include "multiroar.h"

static uint8_t *multiroarPersistSnapshot(const void *structure, size_t *len) {
    const multiroar *r = structure;

    /* Get serialized size first */
    size_t size = multiroarSerializedSize(r);
    cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(size);

    /* Serialize into buffer */
    size_t written = multiroarSerialize(r, buf, size);
    if (written == 0 || written != size) {
        *len = 0;
        return NULL;
    }

    *len = written;

    /* Caller owns the serialized buffer. */
    uint8_t *out = buf;
    buf = NULL;
    return out;
}

static void *multiroarPersistRestore(const uint8_t *data, size_t len) {
    if (!data || len == 0) {
        return NULL;
    }
    /* Deserialize multiroar from snapshot */
    /* @own: caller; free=multiroarFree */
    return multiroarDeserialize(data, len);
}

static size_t multiroarPersistCount(const void *structure) {
    return multiroarBitCount(structure);
}

static size_t multiroarPersistEstimateSize(const void *structure) {
    return multiroarMemoryUsage(structure);
}

/* NOTE: a BITMAP op carries a single raw uint64_t bit position — a fixed-width
 * value with no self-describing framing, so it is NOT a flex-migration target
 * (unlike the multi-element set/list deltas).  Wrapping it in flex would only
 * add header overhead for zero correctness benefit. */
static uint8_t *multiroarPersistEncodeOp(persistOp op, const void *args,
                                         size_t argc, size_t *len) {
    (void)argc;

    switch (op) {
    case PERSIST_OP_INSERT:
    case PERSIST_OP_DELETE: {
        /* args is uint64_t* (position) */
        const uint64_t *position = args;
        *len = sizeof(uint64_t);
        /* @own: caller; free=zfree */
        uint8_t *buf = zmalloc(*len);
        memcpy(buf, position, sizeof(uint64_t));
        return buf;
    }

    case PERSIST_OP_CLEAR_RANGE:
    case PERSIST_OP_FLIP_RANGE: {
        /* args is uint64_t[2] ([start, extent]) — same fixed-width
         * raw framing as the single-position ops. */
        const uint64_t *range = args;
        *len = 2 * sizeof(uint64_t);
        /* @own: caller; free=zfree */
        uint8_t *buf = zmalloc(*len);
        memcpy(buf, range, 2 * sizeof(uint64_t));
        return buf;
    }

    case PERSIST_OP_CLEAR:
        *len = 0;
        return NULL;

    default:
        *len = 0;
        return NULL;
    }
}

static bool multiroarPersistApplyOp(void *structure, persistOp op,
                                    const uint8_t *data, size_t len) {
    multiroar **r = structure; /* Pointer to multiroar pointer */

    switch (op) {
    case PERSIST_OP_INSERT: {
        if (len != sizeof(uint64_t)) {
            return false;
        }
        uint64_t position;
        memcpy(&position, data, sizeof(uint64_t));
        multiroarBitSet(*r, position);
        return true;
    }

    case PERSIST_OP_DELETE: {
        if (len != sizeof(uint64_t)) {
            return false;
        }
        uint64_t position;
        memcpy(&position, data, sizeof(uint64_t));
        multiroarRemove(*r, position);
        return true;
    }

    case PERSIST_OP_CLEAR_RANGE:
    case PERSIST_OP_FLIP_RANGE: {
        if (len != 2 * sizeof(uint64_t)) {
            return false;
        }
        uint64_t range[2];
        memcpy(range, data, sizeof(range));
        if (op == PERSIST_OP_CLEAR_RANGE) {
            multiroarBitClearRange(*r, range[0], range[1]);
        } else {
            multiroarBitFlipRange(*r, range[0], range[1]);
        }
        return true;
    }

    case PERSIST_OP_CLEAR: {
        multiroarFree(*r);
        /* @own: caller; free=multiroarFree */
        *r = multiroarBitNew();
        return true;
    }

    default:
        return false;
    }
}

static void multiroarPersistFree(void *structure) {
    multiroarFree(structure);
}

static void *multiroarPersistNewEmpty(void) {
    /* @own: caller; free=multiroarFree */
    return multiroarBitNew();
}

const persistOps persistOpsMultiroar = {
    .type = PERSIST_TYPE_MULTIROAR,
    .name = "multiroar",
    .snapshot = multiroarPersistSnapshot,
    .restore = multiroarPersistRestore,
    .newEmpty = multiroarPersistNewEmpty,
    .count = multiroarPersistCount,
    .estimateSize = multiroarPersistEstimateSize,
    .encodeOp = multiroarPersistEncodeOp,
    .applyOp = multiroarPersistApplyOp,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = NULL,
    .free = multiroarPersistFree,
};

/* ============================================================================
 * Structure-Specific Operations: hyperloglog
 * ============================================================================
 *
 * The HLL is stored internally as an mdsc-prefixed byte buffer
 * (hyperloglog* and mdsc* are layout-compatible — toHLL/toMDSC inside
 * hyperloglog.c are file-static identity casts). Snapshots are the
 * raw bytes; restore mdscnewlen's a fresh copy; PFADD deltas carry
 * the value bytes that hyperloglogAdd hashes internally.
 */
#include "hyperloglog.h"
#include "mdsc.h"

defcleaner(hyperloglog *, hyperloglogFree);

static uint8_t *hyperloglogPersistSnapshot(const void *structure, size_t *len) {
    const mdsc *m = (const mdsc *)structure;
    size_t l = mdsclen(m);
    /* @own: caller; free=zfree */
    uint8_t *buf = zmalloc(l);
    if (l > 0) {
        memcpy(buf, structure, l);
    }
    *len = l;
    return buf;
}

static void *hyperloglogPersistRestore(const uint8_t *data, size_t len) {
    if (!data || len == 0) {
        return NULL;
    }

    /* The bytes may be damaged: refuse any whose header does not describe
     * a walkable register set before a reader (count, merge) walks it */
    cleaner(hyperloglogFree) hyperloglog *h =
        (hyperloglog *)mdscnewlen(data, len);
    if (!hyperloglogDetect(h)) {
        return NULL;
    }

    /* @own: caller; free=hyperloglogFree */
    hyperloglog *out = h;
    h = NULL;
    return out;
}

static void *hyperloglogPersistNewEmpty(void) {
    /* @own: caller; free=hyperloglogFree */
    return hyperloglogNew();
}

static size_t hyperloglogPersistCount(const void *structure) {
    /* hyperloglogCount needs a non-const handle because it may
     * recompute the cached cardinality. The const-cast matches
     * datakit convention for these read-mostly probes. */
    bool invalid = false;
    return (size_t)hyperloglogCount((hyperloglog *)structure, &invalid);
}

static size_t hyperloglogPersistEstimateSize(const void *structure) {
    return mdsclen((const mdsc *)structure);
}

static uint8_t *hyperloglogPersistEncodeOp(persistOp op, const void *args,
                                           size_t argc, size_t *len) {
    /* For PFADD: args points at the raw value bytes; argc is the
     * byte count. hyperloglogAdd hashes internally, so the WAL
     * carries the original member bytes verbatim.
     *
     * IMPORTANT: the exact member bytes are what the hash sees.  Never encode
     * them through flex's auto-convert push path, which turns a numeral
     * string such as "22" into the integer 22; that would change the bytes
     * fed to the hash and desync the cardinality estimate between leader and
     * follower.  Raw bytes, or flex's exact path, keep them as given.
     *
     * For PERSIST_OP_MERGE: args is the byte image of a source-union HLL
     * (an mdsc-prefixed hyperloglog, the snapshot shape); the fold
     * max-merges it into the destination.  Same verbatim-bytes contract. */
    if (op == PERSIST_OP_INSERT || op == PERSIST_OP_MERGE) {
        uint8_t *buf = NULL;
        if (argc > 0) {
            /* @own: caller; free=zfree */
            buf = zmalloc(argc);
            memcpy(buf, args, argc);
        }
        *len = argc;
        return buf;
    }
    *len = 0;
    return NULL;
}

static bool hyperloglogPersistApplyOp(void *structure, persistOp op,
                                      const uint8_t *data, size_t len) {
    /* structure is hyperloglog**; hyperloglogAdd may reallocate
     * (sparse → dense conversion). */
    hyperloglog **r = structure;
    if (op == PERSIST_OP_INSERT) {
        hyperloglogAdd(r, data, len);
        /* hyperloglogAdd's DENSE arm sets registers without touching
         * the cached cardinality (only the sparse arm invalidates
         * internally) — without this, a dense HLL that absorbed folds
         * keeps serving its stale cached count. */
        hyperloglogInvalidateCache(*r);
        return true;
    }
    if (op == PERSIST_OP_MERGE) {
        /* Empty union is the merge identity (all sources absent):
         * nothing to fold, but the op still creates the destination
         * via the caller's newEmpty path, matching the executor. */
        if (!data || len == 0) {
            return true;
        }
        hyperloglog *src = hyperloglogPersistRestore(data, len);
        if (!src) {
            return false;
        }

        /* WAL bytes are attacker-reachable: the merge walks a fixed
         * dense-register span, so the structure must be proven
         * well-formed before any register read touches it. */
        if (!hyperloglogDetect(src)) {
            hyperloglogFree(src);
            return false;
        }

        const bool ok = hyperloglogMergeInto(r, src);
        hyperloglogFree(src);
        return ok;
    }
    return false;
}

static void hyperloglogPersistFree(void *structure) {
    hyperloglogFree(structure);
}

/* Restored bytes carry an attacker-controlled header; reject anything
 * whose declared encoding and length don't describe a walkable
 * register set before the structure reaches any reader. */
static bool hyperloglogPersistValidate(const void *structure) {
    return hyperloglogDetect((hyperloglog *)structure);
}

const persistOps persistOpsHyperloglog = {
    .type = PERSIST_TYPE_HYPERLOGLOG,
    .name = "hyperloglog",
    .snapshot = hyperloglogPersistSnapshot,
    .restore = hyperloglogPersistRestore,
    .newEmpty = hyperloglogPersistNewEmpty,
    .count = hyperloglogPersistCount,
    .estimateSize = hyperloglogPersistEstimateSize,
    .encodeOp = hyperloglogPersistEncodeOp,
    .applyOp = hyperloglogPersistApplyOp,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = hyperloglogPersistValidate,
    .free = hyperloglogPersistFree,
};

/* ============================================================================
 * Structure-Specific Operations: multimap
 * ============================================================================
 */
#include "multimap.h"

defcleaner(multimap *, multimapFree);

/* Get elementsPerEntry using the iterator interface (avoids internal headers)
 */
/* Set in a snapshot's shape word for a set; elementsPerEntry is at most
 * 1024, so snapshots written before sets were recorded restore as maps,
 * as they always did */
#define MULTIMAP_PERSIST_SET (UINT32_C(1) << 31)

static uint32_t multimapPersistGetElementsPerEntry(const multimap *m) {
    /* The iterator caches elementsPerEntry, so we can use it to extract the
     * value */
    multimapIterator iter;
    multimapIteratorInit(m, &iter,
                         true); /* void return, always sets elementsPerEntry */
    return iter.elementsPerEntry;
}

static uint8_t *multimapPersistSnapshot(const void *structure, size_t *len) {
    const multimap *m = structure;

    /* Get elementsPerEntry from the multimap structure */
    uint32_t elementsPerEntry = multimapPersistGetElementsPerEntry(m);
    if (elementsPerEntry == 0) {
        *len = 0;
        return NULL;
    }

    /* Use multimapDump which returns a flex */
    flex *f = multimapDump(m);
    if (!f) {
        *len = 0;
        return NULL;
    }

    /* Snapshot format: [shape: 4 bytes][flex data: N bytes], where shape
     * is elementsPerEntry with MULTIMAP_PERSIST_SET set for a set */
    const uint32_t shape =
        elementsPerEntry | (multimapIsSet(m) ? MULTIMAP_PERSIST_SET : 0);
    size_t flexLen = flexBytes(f);
    *len = sizeof(uint32_t) + flexLen;
    /* @own: caller; free=zfree */
    uint8_t *buf = zmalloc(*len);

    memcpy(buf, &shape, sizeof(uint32_t));
    memcpy(buf + sizeof(uint32_t), f, flexLen);

    flexFree(f);
    return buf;
}

static void *multimapPersistRestore(const uint8_t *data, size_t len) {
    /* Snapshot format: [shape: 4 bytes][flex data: N bytes] */
    if (!data || len < sizeof(uint32_t)) {
        return NULL;
    }

    uint32_t shape;
    memcpy(&shape, data, sizeof(uint32_t));
    const bool isSet = shape & MULTIMAP_PERSIST_SET;
    const uint32_t elementsPerEntry = shape & ~MULTIMAP_PERSIST_SET;
    if (elementsPerEntry == 0 || elementsPerEntry > 1024) {
        return NULL; /* Sanity check */
    }

    /* Every entry is decoded below, so every entry is checked first */
    size_t flexLen = len - sizeof(uint32_t);
    if (!flexValidateBufferDeep(data + sizeof(uint32_t), flexLen)) {
        return NULL;
    }

    /* The rows, as multimapDump wrote them: a dump that is not whole,
     * strictly ascending rows is refused */
    /* @own: container; free=flexFree */
    flex *rows = zmalloc(flexLen);
    memcpy(rows, data + sizeof(uint32_t), flexLen);

    /* @own: caller; free=multimapFree */
    return multimapNewFromDump(elementsPerEntry, isSet, rows);
}

static size_t multimapPersistCount(const void *structure) {
    return multimapCount(structure);
}

static size_t multimapPersistEstimateSize(const void *structure) {
    return multimapBytes(structure);
}

static uint8_t *multimapPersistEncodeOp(persistOp op, const void *args,
                                        size_t argc, size_t *len) {
    /* args is const databox** for multimap operations */
    const databox *const *boxes = args;

    switch (op) {
    case PERSIST_OP_INSERT: {
        /* All argc member boxes as a self-describing flex (no [argc] byte, no
         * per-type length games — flex records each element's length, which
         * the old hand-rolled multi-value path could not do for a non-last
         * BYTES value). */
        if (argc == 0 || !boxes) {
            *len = 0;
            return NULL;
        }
        flex *f = flexNewFromDataboxPtrsExact(boxes, argc);
        if (!f) {
            *len = 0;
            return NULL;
        }
        *len = flexBytes(f);
        return (uint8_t *)f;
    }

    case PERSIST_OP_DELETE: {
        /* One element: delete by key.  The full row (multimapDeleteFullWidth,
         * which picks one row of a set's key): every element. */
        if (argc < 1 || !boxes || !boxes[0]) {
            *len = 0;
            return NULL;
        }
        flex *f = flexNewFromDataboxPtrsExact(boxes, argc);
        if (!f) {
            *len = 0;
            return NULL;
        }
        *len = flexBytes(f);
        return (uint8_t *)f;
    }

    case PERSIST_OP_NUM_INCR: {
        /* multimapFieldIncr: args are [key, field offset (unsigned), delta
         * (signed)], stored as a 3-element flex */
        if (argc != 3 || !boxes || !boxes[0] || !boxes[1] || !boxes[2]) {
            *len = 0;
            return NULL;
        }
        flex *f = flexNewFromDataboxPtrsExact(boxes, argc);
        if (!f) {
            *len = 0;
            return NULL;
        }
        *len = flexBytes(f);
        return (uint8_t *)f;
    }

    case PERSIST_OP_CLEAR:
        *len = 0;
        return NULL;

    default:
        *len = 0;
        return NULL;
    }
}

/* An int64 operand of a WAL row: flex stores an integer by value, so a
 * non-negative one decodes as unsigned whichever way it was written */
static bool multimapPersistInt64(const databoxBig *b, int64_t *out) {
    if (b->type == DATABOX_SIGNED_64) {
        *out = b->data.i64;
        return true;
    }
    if (b->type == DATABOX_UNSIGNED_64 && b->data.u64 <= INT64_MAX) {
        *out = (int64_t)b->data.u64;
        return true;
    }
    return false;
}

/* Decode a WAL row (a flex of up to 32 elements, from untrusted bytes)
 * into `boxes`, with `ptrs` aimed at them.  databoxBig so a 96/128-bit
 * element decodes into storage the box owns. */
static bool multimapPersistDecodeRow(const uint8_t *data, size_t len,
                                     databoxBig boxes[32],
                                     const databox *ptrs[32], size_t *count) {
    if (!flexValidateBufferDeep(data, len)) {
        return false;
    }
    const flex *f = (const flex *)data;
    *count = flexCount(f);
    if (*count == 0 || *count > 32) {
        return false;
    }
    flexEntry *fe = flexHead(f);
    for (size_t i = 0; i < *count; i++) {
        flexGetByTypeBig(fe, &boxes[i]);
        ptrs[i] = (const databox *)&boxes[i];
        fe = flexNext(f, fe);
    }
    return true;
}

/* Helper to determine encoded value length from databoxLinear type byte */
static size_t databoxLinearTypeValueLen(uint8_t type) {
    /* Type byte encoding (from databoxLinear.c):
     * 0: invalid
     * 1: BYTES (variable, handled separately)
     * 2-17: integers (2-3 = 1 byte, 4-5 = 2 bytes, ... 16-17 = 8 bytes)
     * 18: REAL_16B (2 bytes)
     * 19: REAL_32B (4 bytes)
     * 20: REAL_64B (8 bytes)
     * 21-23: TRUE/FALSE/NULL (0 bytes)
     */
    if (type >= 2 && type <= 17) {
        /* Integer types: ((type - 2) / 2) + 1 bytes */
        return ((type - 2) / 2) + 1;
    } else if (type == 18) {
        return 2; /* REAL_16B */
    } else if (type == 19) {
        return 4; /* REAL_32B */
    } else if (type == 20) {
        return 8; /* REAL_64B */
    } else if (type >= 21 && type <= 23) {
        return 0; /* TRUE/FALSE/NULL */
    }
    return 0; /* Invalid or BYTES */
}

static bool multimapPersistApplyOp(void *structure, persistOp op,
                                   const uint8_t *data, size_t len) {
    multimap **m = structure;
    databoxBig boxes[32];
    const databox *ptrs[32];
    size_t count;

    switch (op) {
    case PERSIST_OP_INSERT:
        /* A full row: anything else would leave elements unread */
        if (!multimapPersistDecodeRow(data, len, boxes, ptrs, &count) ||
            count != multimapPersistGetElementsPerEntry(*m)) {
            return false;
        }
        multimapInsert(m, ptrs);
        return true;

    case PERSIST_OP_DELETE:
        if (!multimapPersistDecodeRow(data, len, boxes, ptrs, &count)) {
            return false;
        }
        if (count == 1) {
            multimapDelete(m, ptrs[0]);
        } else if (count == multimapPersistGetElementsPerEntry(*m)) {
            multimapDeleteFullWidth(m, ptrs);
        } else {
            return false;
        }
        return true;

    case PERSIST_OP_NUM_INCR: {
        int64_t offset;
        int64_t delta;
        if (!multimapPersistDecodeRow(data, len, boxes, ptrs, &count) ||
            count != 3 || !multimapPersistInt64(&boxes[1], &offset) ||
            offset < 0 || offset > UINT32_MAX ||
            !multimapPersistInt64(&boxes[2], &delta)) {
            return false;
        }
        /* A refused increment (missing key, not a counter, outside int64)
         * was refused the same way where it was logged, so replaying the
         * refusal is the faithful replay */
        int64_t ignored;
        multimapFieldIncrBy(m, ptrs[0], (uint32_t)offset, delta, &ignored);
        return true;
    }

    case PERSIST_OP_CLEAR: {
        multimapReset(*m);
        return true;
    }

    default:
        return false;
    }
}

static void multimapPersistFree(void *structure) {
    multimapFree(structure);
}

static void *multimapPersistNewEmpty(void) {
    return multimapSetNew(1);
}

const persistOps persistOpsMultimap = {
    .deletesWhenEmpty = true,
    .type = PERSIST_TYPE_MULTIMAP,
    .name = "multimap",
    .snapshot = multimapPersistSnapshot,
    .restore = multimapPersistRestore,
    .newEmpty = multimapPersistNewEmpty,
    .count = multimapPersistCount,
    .estimateSize = multimapPersistEstimateSize,
    .encodeOp = multimapPersistEncodeOp,
    .applyOp = multimapPersistApplyOp,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = NULL,
    .free = multimapPersistFree,
};

/* ============================================================================
 * Structure-Specific Operations: multilist
 * ============================================================================
 */
#include "multilist.h"

defcleaner(multilist *, multilistFree);

static uint8_t *multilistPersistSnapshot(const void *structure, size_t *len) {
    const multilist *ml = structure;

    /* Every element in order as one flex.  The dump copies the element
     * encodings without decoding them, so its bytes are exactly those of
     * pushing each element in turn, and 96/128-bit elements need no
     * special handling. */
    cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
    cleaner(flexFree) flex *f = multilistDump(ml, state);

    *len = flexBytes(f);
    /* @own: caller; free=zfree */
    uint8_t *buf = zmalloc(*len);
    memcpy(buf, f, *len);
    return buf;
}

static void *multilistPersistRestore(const uint8_t *data, size_t len) {
    /* Every element is decoded below, so validate each entry, not just the
     * header. */
    if (!flexValidateBufferDeep(data, len)) {
        return NULL;
    }

    /* Restore the flex */
    cleaner(flexFree) flex *f = zmalloc(len);
    memcpy(f, data, len);

    /* Create multilist and populate from flex */
    cleaner(multilistFree) multilist *ml =
        multilistNew(FLEX_CAP_LEVEL_2048, 0); /* default config */
    /* Use thread-local replay state set by persistRecover. That state is
     * borrowed; only a locally created fallback is owned here. */
    cleaner(mflexStateFree) mflexState *ownedState = NULL;
    mflexState *state = persistGetReplayState();
    if (!state) {
        /* If not in replay context, create temporary state */
        ownedState = mflexStateCreate();
        if (!ownedState) {
            return NULL;
        }
        state = ownedState;
    }

    /* Iterate through flex and push each element to multilist
     * Note: flexHead returns non-NULL even for empty flex (points to end
     * marker), so we must check count to know when to stop */
    size_t count = flexCount(f);
    flexEntry *fe = flexHead(f);
    for (size_t i = 0; i < count; i++) {
        databoxBig box; /* room for a 96/128-bit element */
        flexGetByTypeBig(fe, &box);
        multilistPushByTypeTail(&ml, state, (databox *)&box);
        fe = flexNext(f, fe);
    }

    /* @own: caller; free=multilistFree */
    multilist *out = ml;
    ml = NULL;
    return out;
}

static size_t multilistPersistCount(const void *structure) {
    return multilistCount(structure);
}

static size_t multilistPersistEstimateSize(const void *structure) {
    return multilistBytes(structure);
}

static uint8_t *multilistPersistEncodeOp(persistOp op, const void *args,
                                         size_t argc, size_t *len) {
    switch (op) {
    case PERSIST_OP_PUSH_HEAD:
    case PERSIST_OP_PUSH_TAIL: {
        /* args is a single const databox* — the pushed value — as a 1-element
         * flex. */
        flex *f = flexNewFromDataboxExact((const databox *)args);
        if (!f) {
            *len = 0;
            return NULL;
        }
        *len = flexBytes(f);
        return (uint8_t *)f;
    }

    case PERSIST_OP_POP_HEAD:
    case PERSIST_OP_POP_TAIL:
    case PERSIST_OP_CLEAR:
        /* No arguments needed */
        *len = 0;
        return NULL;

    case PERSIST_OP_ROTATE:
        /* No arguments needed */
        *len = 0;
        return NULL;

    case PERSIST_OP_DELETE_AT:
    case PERSIST_OP_TRIM:
    case PERSIST_OP_REPLACE: {
        /* args is const databox*[2]: DELETE_AT = [start, count], TRIM =
         * [head_cut, tail_cut], REPLACE = [index, value] — all as a
         * 2-element flex. */
        if (argc < 2) {
            *len = 0;
            return NULL;
        }
        flex *f = flexNewFromDataboxPtrsExact((const databox *const *)args, 2);
        if (!f) {
            *len = 0;
            return NULL;
        }
        *len = flexBytes(f);
        return (uint8_t *)f;
    }

    default:
        *len = 0;
        return NULL;
    }
}

/* Identity-verified POP apply.
 *
 * A POP record may carry the element the origin removed, encoded as a
 * 1-element flex like a PUSH operand.  A plain end-pop is exact about
 * the count but not about which element: applying the same pop twice
 * removes whatever is at the end by then, turning a duplicate into a
 * lost element elsewhere in the list.  With the operand, replay is
 * exact:
 *   (1) the popping end holds it: pop it, the usual case;
 *   (2) it is elsewhere in the list: remove that element, which happens
 *       when independent pops are applied out of order;
 *   (3) it is absent: the pop was already applied, so the record is a
 *       counted, idempotent no-op.
 * Pops without an operand keep the plain positional behaviour. */
static _Atomic uint64_t mlPopIdentityDeepRemoveTotal = 0; /* arm 2 */
static _Atomic uint64_t mlPopIdentityAbsentNoopTotal = 0; /* arm 3 */

uint64_t persistMultilistPopIdentityDeepRemoveTotal(void) {
    return atomic_load_explicit(&mlPopIdentityDeepRemoveTotal,
                                memory_order_relaxed);
}

uint64_t persistMultilistPopIdentityAbsentNoopTotal(void) {
    return atomic_load_explicit(&mlPopIdentityAbsentNoopTotal,
                                memory_order_relaxed);
}

static bool multilistApplyPopIdentity(multilist **ml, mflexState *state,
                                      const databox *want, bool fromTail) {
    /* The key may not exist yet (an external coordinator can drive
     * this apply path against a not-yet-created structure) */
    const size_t cnt = *ml ? multilistCount(*ml) : 0;
    if (cnt == 0) {
        atomic_fetch_add_explicit(&mlPopIdentityAbsentNoopTotal, 1,
                                  memory_order_relaxed);
        return true;
    }
    /* The Wide read, so a 96/128-bit end element compares by value. */
    multilistEntry end;
    databoxWide endWide;
    /* Identity, not IEEE equality: a NaN element must match its pop. */
    if (multilistIndexWide(*ml, state, fromTail ? (mlOffsetId)(cnt - 1) : 0,
                           &end, &endWide) &&
        databoxCompare(&end.box, want) == 0) {
        /* The popped value is an owned copy nobody needs. */
        databox got = {{0}};
        const bool popped = multilistPop(ml, state, &got, fromTail);
        databoxFreeData(&got);
        return popped;
    }
    /* Not at the popping end — search FROM that end and remove by
     * identity.  The iterator wants two scratch states; the replay
     * thread-local provides one, borrow a temporary for the second
     * (the ROTATE arm's idiom). */
    cleaner(mflexStateFree) mflexState *tmp = mflexStateCreate();
    if (!tmp) {
        return false;
    }
    mflexState *pair[2] = {state, tmp};
    multilistIterator iter = {0};
    bool removed = false;
    if (fromTail) {
        multilistIteratorInitReverse(*ml, pair, &iter);
    } else {
        multilistIteratorInitForward(*ml, pair, &iter);
    }
    multilistEntry entry = {0};
    while (multilistNext(&iter, &entry)) {
        if (databoxCompare(&entry.box, want) == 0) {
            multilistDelEntry(&iter, &entry);
            removed = true;
            break;
        }
    }

    /* Deletions on a compressed node land in the shared scratch buffer
     * and only reach the node itself when the iterator is released;
     * skipping this drops the removal and leaves the node pointer
     * stale after a reallocating delete. */
    multilistIteratorRelease(&iter);
    atomic_fetch_add_explicit(removed ? &mlPopIdentityDeepRemoveTotal
                                      : &mlPopIdentityAbsentNoopTotal,
                              1, memory_order_relaxed);
    /* Absent = already applied: an idempotent no-op, never an
     * error — failing the apply here would wedge replay on exactly
     * the duplicate this arm exists to absorb. */
    return true;
}

static bool multilistPersistApplyOp(void *structure, persistOp op,
                                    const uint8_t *data, size_t len) {
    multilist **ml = structure;
    /* Use thread-local replay state set by persistReplayWAL */
    mflexState *state = persistGetReplayState();

    switch (op) {
    case PERSIST_OP_PUSH_HEAD:
    case PERSIST_OP_PUSH_TAIL: {
        /* databoxBig: the pushed value may be 96/128-bit */
        databoxBig box;
        if (!flexGetSingleDataboxBig(data, len, &box)) {
            return false;
        }
        if (op == PERSIST_OP_PUSH_HEAD) {
            multilistPushByTypeHead(ml, state, (const databox *)&box);
        } else {
            multilistPushByTypeTail(ml, state, (const databox *)&box);
        }
        return true;
    }

    case PERSIST_OP_POP_HEAD:
    case PERSIST_OP_POP_TAIL: {
        const bool fromTail = (op == PERSIST_OP_POP_TAIL);
        /* databoxBig: the identity operand may be 96/128-bit */
        databoxBig want;
        if (data && len && flexGetSingleDataboxBig(data, len, &want)) {
            return multilistApplyPopIdentity(ml, state, (const databox *)&want,
                                             fromTail);
        }
        /* An operand-less pop on an empty list is a no-op, never a
         * rejection, matching the identity arm's absent rule above and
         * the live pop, which returns nothing on an empty list.  A writer
         * may log a pop before it knows whether the list has an element;
         * refusing the record would make replay disagree with the live
         * result and stop recovery. */
        if (!*ml || multilistCount(*ml) == 0) {
            return true;
        }
        /* The popped value is an owned copy nobody needs. */
        databox got = {{0}};
        const bool popped = multilistPop(ml, state, &got, fromTail);
        databoxFreeData(&got);
        return popped;
    }

    case PERSIST_OP_DELETE_AT: {
        /* [start, count] as a 2-element flex. */
        databox startBox, countBox;
        if (!flexGetTwoDataboxes(data, len, &startBox, &countBox)) {
            return false;
        }
        /* Both operands are int64s at the origin. */
        if ((startBox.type != DATABOX_SIGNED_64 &&
             startBox.type != DATABOX_UNSIGNED_64) ||
            (countBox.type != DATABOX_SIGNED_64 &&
             countBox.type != DATABOX_UNSIGNED_64) ||
            (startBox.type == DATABOX_UNSIGNED_64 &&
             startBox.data.u > (uint64_t)INT64_MAX) ||
            (countBox.type == DATABOX_UNSIGNED_64 &&
             countBox.data.u > (uint64_t)INT64_MAX)) {
            return false;
        }
        /* Replay runs against the state the origin had, so a range that
         * deletes nothing here deleted nothing there: a no-op, not a
         * failure.  Older writers logged such calls. */
        (void)multilistDelRange(ml, state, startBox.data.i, countBox.data.i);
        return true;
    }

    case PERSIST_OP_TRIM: {
        /* [head_cut, tail_cut] as a 2-element flex — the normalized
         * element counts the origin's LTRIM removed from each end. */
        databox headBox, tailBox;
        if (!flexGetTwoDataboxes(data, len, &headBox, &tailBox)) {
            return false;
        }
        const int64_t headCut = (int64_t)headBox.data.u;
        const int64_t tailCut = (int64_t)tailBox.data.u;
        if (headCut > 0) {
            multilistDelRange(ml, state, 0, headCut);
        }
        if (tailCut > 0) {
            multilistDelRange(ml, state, -tailCut, tailCut);
        }
        return true;
    }

    case PERSIST_OP_ROTATE: {
        /* No operand.  Rotating a 0/1-element list is a no-op.
         * multilistRotate stages the popped tail and the pushed head
         * through TWO mflex scratch states — the replay thread-local
         * provides one; borrow a temporary for the second. */
        if (multilistCount(*ml) > 1) {
            if (!state) {
                return false;
            }
            cleaner(mflexStateFree) mflexState *tmp = mflexStateCreate();
            if (!tmp) {
                return false;
            }
            mflexState *pair[2] = {state, tmp};
            multilistRotate(*ml, pair);
        }
        return true;
    }

    case PERSIST_OP_REPLACE: {
        /* [index, value] as a 2-element flex; the value may be
         * 96/128-bit. */
        databoxBig indexBox, valueBox;
        if (!flexGetTwoDataboxesBig(data, len, &indexBox, &valueBox)) {
            return false;
        }
        if (indexBox.type != DATABOX_SIGNED_64 &&
            indexBox.type != DATABOX_UNSIGNED_64) {
            return false;
        }
        /* The origin's index is an int64: an unsigned operand above
         * INT64_MAX is malformed, not a negative index. */
        if (indexBox.type == DATABOX_UNSIGNED_64 &&
            indexBox.data.u > (uint64_t)INT64_MAX) {
            return false;
        }

        /* As for DELETE_AT: an index the origin refused is refused here
         * too, so the record is a no-op. */
        (void)multilistReplaceByTypeAtIndex(ml, state, indexBox.data.i,
                                            (const databox *)&valueBox);
        return true;
    }

    case PERSIST_OP_CLEAR:
        multilistFree(*ml);
        /* @own: caller; free=multilistFree */
        *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
        return true;

    default:
        return false;
    }
}

static void multilistPersistFree(void *structure) {
    multilistFree(structure);
}

static void *multilistPersistNewEmpty(void) {
    /* @own: caller; free=multilistFree */
    return multilistNew(FLEX_CAP_LEVEL_16384, 2);
}

const persistOps persistOpsMultilist = {
    .deletesWhenEmpty = true,
    .type = PERSIST_TYPE_MULTILIST,
    .name = "multilist",
    .snapshot = multilistPersistSnapshot,
    .restore = multilistPersistRestore,
    .newEmpty = multilistPersistNewEmpty,
    .count = multilistPersistCount,
    .estimateSize = multilistPersistEstimateSize,
    .encodeOp = multilistPersistEncodeOp,
    .applyOp = multilistPersistApplyOp,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = NULL,
    .free = multilistPersistFree,
};

/* ============================================================================
 * Structure-Specific Operations: multidict
 * ============================================================================
 */
#include "multidict.h"

/* A multidict snapshot is a flex of [key, val, key, val, ...].  A dict
 * whose keys are not exact records how they match as one leading
 * unsigned element (mdPersistKeys), which makes the element count odd.
 * An even count is a snapshot without that element: an exact-key dict,
 * which is also what every snapshot written before the element existed
 * restores as.  An exact-key dict still writes no element, so its
 * snapshots keep the original layout.  The WAL needs no element: its
 * operations replay onto the dict the snapshot restores, and every
 * recovery starts from a snapshot. */
typedef enum mdPersistKeys {
    MD_PERSIST_KEYS_EXACT = 0,  /* multidictTypeExactKey */
    MD_PERSIST_KEYS_CASE = 1,   /* multidictTypeCaseKey */
    MD_PERSIST_KEYS_CUSTOM = 2, /* any other type: the caller supplies it */
} mdPersistKeys;

static mdPersistKeys mdPersistKeysOf(const multidictType *type) {
    if (type == &multidictTypeExactKey) {
        return MD_PERSIST_KEYS_EXACT;
    }

    if (type == &multidictTypeCaseKey) {
        return MD_PERSIST_KEYS_CASE;
    }

    return MD_PERSIST_KEYS_CUSTOM;
}

static uint8_t *multidictPersistSnapshot(const void *structure, size_t *len) {
    const multidict *d = structure;

    /* Create a flex to hold all key-value pairs */
    cleaner(flexFree) flex *f = flexNew();

    const mdPersistKeys keys = mdPersistKeysOf(multidictGetType(d));
    if (keys != MD_PERSIST_KEYS_EXACT) {
        const databox tag = DATABOX_UNSIGNED(keys);
        flexPushByTypeExact(&f, &tag, FLEX_ENDPOINT_TAIL);
    }

    /* Iterate through all entries and add to flex as [key, val, key, val, ...]
     */
    multidictIterator iter;
    if (multidictIteratorInit((multidict *)d, &iter)) {
        multidictEntry entry;
        while (multidictIteratorNext(&iter, &entry)) {
            const databox *pair[2] = {&entry.key, &entry.val};
            flexAppendMultipleExact(&f, 2, pair);
        }
        multidictIteratorRelease(&iter);
    }

    *len = flexBytes(f);
    /* @own: caller; free=zfree */
    uint8_t *buf = zmalloc(*len);
    memcpy(buf, f, *len);
    return buf;
}

/* Restore a multidict snapshot.  'want' NULL takes the type from the
 * snapshot and accepts only the built-in types; otherwise the snapshot
 * must have been written by a dict of that kind (exact, case, or a
 * custom type), and any mismatch is refused. */
static void *multidictPersistRestoreTyped_(const uint8_t *data, size_t len,
                                           multidictType *want) {
    if (!flexValidateBuffer(data, len)) {
        return NULL;
    }
    /* Restore the flex */
    cleaner(flexFree) flex *f = zmalloc(len);
    memcpy(f, data, len);

    /* Note: flexHead returns non-NULL even for empty flex, so we must
     * check count */
    size_t count = flexCount(f);
    flexEntry *fe = flexHead(f);

    /* An odd count leads with the key-matching element */
    mdPersistKeys keys = MD_PERSIST_KEYS_EXACT;
    if (count % 2 == 1) {
        databox tag = {{0}};
        flexGetByType(fe, &tag);
        if (!DATABOX_IS_INTEGER(&tag) || tag.data.u > MD_PERSIST_KEYS_CUSTOM) {
            return NULL; /* not a snapshot this code wrote */
        }

        keys = (mdPersistKeys)tag.data.u;
        fe = flexNext(f, fe);
        count--;
    }

    multidictType *type;
    if (want) {
        if (mdPersistKeysOf(want) != keys) {
            return NULL; /* written by a dict that matched keys otherwise */
        }
        type = want;
    } else if (keys == MD_PERSIST_KEYS_CUSTOM) {
        return NULL; /* needs the caller's type: multidictPersistRestoreAs */
    } else {
        type = keys == MD_PERSIST_KEYS_CASE ? &multidictTypeCaseKey
                                            : &multidictTypeExactKey;
    }

    /* Create multidict with the type and a default class. The dict
     * privately owns the class; the restore contract hands both to the
     * caller, who releases the class after the dict (see
     * multidictPersistFree). */
    /* @own: container; free=multidictDefaultClassFree */
    multidictClass *qdc = multidictDefaultClassNew();
    /* @own: caller; free=multidictFree */
    multidict *d = multidictNew(type, qdc, 0);

    /* Iterate through flex and add each key-value pair */
    for (size_t i = 0; i + 1 < count; i += 2) {
        databoxBig key, val; /* room for 96/128-bit keys and values */

        /* Get key */
        flexGetByTypeBig(fe, &key);
        fe = flexNext(f, fe);

        /* Get value */
        flexGetByTypeBig(fe, &val);
        fe = flexNext(f, fe);

        /* Add to multidict */
        multidictAdd(d, (databox *)&key, (databox *)&val);
    }

    return d;
}

static void *multidictPersistRestore(const uint8_t *data, size_t len) {
    return multidictPersistRestoreTyped_(data, len, NULL);
}

void *multidictPersistRestoreAs(const uint8_t *data, size_t len,
                                struct multidictType *type) {
    if (!type) {
        return NULL;
    }

    return multidictPersistRestoreTyped_(data, len, type);
}

static size_t multidictPersistCount(const void *structure) {
    return multidictCount(structure);
}

static size_t multidictPersistEstimateSize(const void *structure) {
    return multidictBytes((multidict *)structure);
}

/* A multidict WAL record for an insert, replace or delete is this marker
 * byte followed by a self-describing flex of [key, val] or [key], so keys
 * and values of every type (byte strings of any length, 96/128-bit
 * integers) replay intact.  The marker is no databoxLinear type byte, so
 * a record written before it existed is told apart by its first byte and
 * still decodes: [keyType][key][valType][val] or [keyType][key]. */
#define MD_PERSIST_OP_FLEX ((uint8_t)0xF1)

/* A marked record of 'f'; NULL when 'f' could not be built */
static uint8_t *mdPersistOpRecord_(flex *f, size_t *len) {
    if (!f) {
        *len = 0;
        return NULL;
    }

    *len = 1 + flexBytes(f);
    /* @own: caller; free=zfree */
    uint8_t *buf = zmalloc(*len);
    buf[0] = MD_PERSIST_OP_FLEX;
    memcpy(buf + 1, f, flexBytes(f));
    flexFree(f);
    return buf;
}

static uint8_t *multidictPersistEncodeOp(persistOp op, const void *args,
                                         size_t argc, size_t *len) {
    switch (op) {
    case PERSIST_OP_INSERT:
    case PERSIST_OP_REPLACE: {
        /* args is const databox*[] with [key, val] */
        if (argc < 2 || !args) {
            *len = 0;
            return NULL;
        }
        return mdPersistOpRecord_(flexNewFromDataboxPtrsExact(args, 2), len);
    }

    case PERSIST_OP_DELETE: {
        /* args is const databox* (key only) */
        if (!args) {
            *len = 0;
            return NULL;
        }
        return mdPersistOpRecord_(flexNewFromDataboxExact(args), len);
    }

    case PERSIST_OP_CLEAR:
        *len = 0;
        return NULL;

    default:
        *len = 0;
        return NULL;
    }
}

static bool multidictPersistApplyOp(void *structure, persistOp op,
                                    const uint8_t *data, size_t len) {
    multidict **d = structure;

    switch (op) {
    case PERSIST_OP_INSERT:
    case PERSIST_OP_REPLACE: {
        if (len < 2) {
            return false;
        }

        databoxBig keyBig;
        databoxBig valBig;
        const databox *key = (const databox *)&keyBig;
        const databox *val = (const databox *)&valBig;
        databox oldKey;
        databox oldVal;
        if (data[0] == MD_PERSIST_OP_FLEX) {
            if (!flexGetTwoDataboxesBig(data + 1, len - 1, &keyBig, &valBig)) {
                return false;
            }
        } else {
            /* An unmarked record: [keyType][key][valType][val], whose key
             * had to be fixed-width (a byte-string key carried no length,
             * so no such record could be read back) */
            size_t offset = 0;
            const uint8_t keyType = data[offset++];
            const size_t keyLen = databoxLinearTypeValueLen(keyType);
            if (DATABOX_LINEAR_TYPE_IS_BYTES(keyType) ||
                offset + keyLen > len - 1) {
                return false;
            }
            DATABOX_LINEAR_PARTS_DECODE(keyType, data + offset, keyLen,
                                        &oldKey);
            offset += keyLen;

            /* The value is last, so a byte-string value is the rest */
            const uint8_t valType = data[offset++];
            const size_t valLen = DATABOX_LINEAR_TYPE_IS_BYTES(valType)
                                      ? len - offset
                                      : databoxLinearTypeValueLen(valType);
            if (offset + valLen > len) {
                return false;
            }
            DATABOX_LINEAR_PARTS_DECODE(valType, data + offset, valLen,
                                        &oldVal);
            key = &oldKey;
            val = &oldVal;
        }

        if (op == PERSIST_OP_REPLACE) {
            multidictReplace(*d, key, val);
        } else {
            multidictAdd(*d, key, val);
        }
        return true;
    }

    case PERSIST_OP_DELETE: {
        if (len < 1) {
            return false;
        }

        databoxBig keyBig;
        databox oldKey;
        const databox *key = (const databox *)&keyBig;
        if (data[0] == MD_PERSIST_OP_FLEX) {
            if (!flexGetSingleDataboxBig(data + 1, len - 1, &keyBig)) {
                return false;
            }
        } else {
            /* An unmarked record: [keyType][key], the key the rest */
            if (!DATABOX_LINEAR_TYPE_IS_BYTES(data[0]) &&
                databoxLinearTypeValueLen(data[0]) > len - 1) {
                return false;
            }
            DATABOX_LINEAR_PARTS_DECODE(data[0], data + 1, len - 1, &oldKey);
            key = &oldKey;
        }
        multidictDelete(*d, key);
        return true;
    }

    case PERSIST_OP_CLEAR:
        multidictEmpty(*d);
        return true;

    default:
        return false;
    }
}

static void multidictPersistFree(void *structure) {
    /* Restore created this dict with its own default class; the class
     * must outlive the dict (teardown runs class callbacks), so free
     * it after the dict. */
    multidictClass *cls = multidictGetClass(structure);
    multidictFree(structure);
    multidictDefaultClassFree(cls);
}

const persistOps persistOpsMultidict = {
    .type = PERSIST_TYPE_MULTIDICT,
    .name = "multidict",
    .snapshot = multidictPersistSnapshot,
    .restore = multidictPersistRestore,
    .count = multidictPersistCount,
    .estimateSize = multidictPersistEstimateSize,
    .encodeOp = multidictPersistEncodeOp,
    .applyOp = multidictPersistApplyOp,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = NULL,
    .free = multidictPersistFree,
};

/* ============================================================================
 * Structure-Specific Operations: multiarray
 * ============================================================================
 *
 * Snapshot: a flex [element width, entries per node, every element's bytes
 * in order as one blob].  WAL (args: const databox *const *):
 *   INSERT_AT [index, element bytes]   insert before index (count appends)
 *   DELETE_AT [index]
 *   UPDATE    [index, element bytes]   an element rewritten in place
 *   CLEAR
 * The shape is fixed at creation, so there is no newEmpty.
 */
#include "multiarray.h"

defcleaner(multiarray *, multiarrayFree);

static uint8_t *multiarrayPersistSnapshot(const void *structure, size_t *len) {
    const multiarray *m = structure;
    const uint16_t width = multiarrayElementLen(m);
    const size_t bytes = multiarrayCount(m) * width;
    cleanerName(zfree, Bytes) uint8_t *blob = zmalloc(bytes ? bytes : 1);
    multiarrayCopyTo(m, blob);

    const databox w = DATABOX_UNSIGNED(width);
    const databox r = DATABOX_UNSIGNED(multiarrayRowMax(m));
    const databox b = DATABOX_WITH_BYTES(blob, bytes);
    const databox *parts[] = {&w, &r, &b};
    /* @own: caller; free=zfree */
    flex *f = flexNewFromDataboxPtrsExact(parts, 3);
    *len = flexBytes(f);
    return (uint8_t *)f;
}

/* An unsigned operand up to 'max' */
static bool multiarrayPersistUnsigned_(const databoxBig *b, uint64_t max,
                                       uint64_t *out) {
    if (b->type != DATABOX_UNSIGNED_64 || b->data.u64 > max) {
        return false;
    }
    *out = b->data.u64;
    return true;
}

/* A bytes operand, pointing into the decoded flex */
static bool multiarrayPersistBytes_(databoxBig *b, const uint8_t **bytes,
                                    size_t *len) {
    uint8_t *start;
    if ((b->type != DATABOX_BYTES && b->type != DATABOX_BYTES_EMBED) ||
        !databoxGetBytes((databox *)b, &start, len)) {
        return false;
    }
    *bytes = start;
    return true;
}

static void *multiarrayPersistRestore(const uint8_t *data, size_t len) {
    if (!flexValidateBufferDeep(data, len)) {
        return NULL;
    }
    const flex *f = (const flex *)data;
    if (flexCount(f) != 3) {
        return NULL;
    }

    databoxBig parts[3];
    flexEntry *fe = flexHead(f);
    for (size_t i = 0; i < 3; i++) {
        flexGetByTypeBig(fe, &parts[i]);
        fe = flexNext(f, fe);
    }

    uint64_t width;
    uint64_t rowMax;
    const uint8_t *blob;
    size_t blobLen;
    if (!multiarrayPersistUnsigned_(&parts[0], UINT16_MAX, &width) ||
        width == 0 ||
        !multiarrayPersistUnsigned_(&parts[1], UINT16_MAX, &rowMax) ||
        rowMax == 0 || !multiarrayPersistBytes_(&parts[2], &blob, &blobLen) ||
        blobLen % width) {
        return NULL;
    }

    /* @own: caller; free=multiarrayFree */
    return multiarrayNewFromArray((uint16_t)width, (uint16_t)rowMax, blob,
                                  blobLen / width);
}

static size_t multiarrayPersistCount(const void *structure) {
    return multiarrayCount(structure);
}

static size_t multiarrayPersistEstimateSize(const void *structure) {
    return multiarrayBytes(structure);
}

static uint8_t *multiarrayPersistEncodeOp(persistOp op, const void *args,
                                          size_t argc, size_t *len) {
    const databox *const *boxes = args;
    *len = 0;

    const size_t want = op == PERSIST_OP_INSERT_AT   ? 2
                        : op == PERSIST_OP_UPDATE    ? 2
                        : op == PERSIST_OP_DELETE_AT ? 1
                                                     : 0;
    if (want == 0 || argc != want || !boxes) {
        /* CLEAR has no operand; nothing else is replayed */
        return NULL;
    }

    flex *f = flexNewFromDataboxPtrsExact(boxes, argc);
    if (f) {
        *len = flexBytes(f);
    }
    return (uint8_t *)f;
}

static bool multiarrayPersistApplyOp(void *structure, persistOp op,
                                     const uint8_t *data, size_t len) {
    multiarray **m = structure;
    const size_t count = multiarrayCount(*m);
    const uint16_t width = multiarrayElementLen(*m);

    if (op == PERSIST_OP_CLEAR) {
        const uint16_t rowMax = multiarrayRowMax(*m);
        multiarrayFree(*m);
        /* @own: caller; free=multiarrayFree */
        *m = multiarrayNewFromArray(width, rowMax, NULL, 0);
        return true;
    }

    if (op == PERSIST_OP_DELETE_AT) {
        databoxBig idx;
        uint64_t at;
        if (!flexGetSingleDataboxBig(data, len, &idx) || !count ||
            !multiarrayPersistUnsigned_(&idx, count - 1, &at)) {
            return false;
        }
        multiarrayDelete(m, (uint32_t)at);
        return true;
    }

    if (op != PERSIST_OP_INSERT_AT && op != PERSIST_OP_UPDATE) {
        return false;
    }

    /* [index, element bytes]: an insert may append (index == count); an
     * update rewrites an existing element */
    databoxBig idx;
    databoxBig value;
    uint64_t at;
    const uint8_t *bytes;
    size_t bytesLen;
    if (!flexGetTwoDataboxesBig(data, len, &idx, &value) ||
        !multiarrayPersistUnsigned_(
            &idx, op == PERSIST_OP_INSERT_AT ? count : count - 1, &at) ||
        (op == PERSIST_OP_UPDATE && !count) ||
        !multiarrayPersistBytes_(&value, &bytes, &bytesLen) ||
        bytesLen != width || count >= UINT32_MAX) {
        return false;
    }

    if (op == PERSIST_OP_INSERT_AT) {
        multiarrayInsert(m, (multiarrayIdx)at, (void *)bytes);
    } else {
        memcpy(multiarrayGet(*m, (multiarrayIdx)at), bytes, width);
    }
    return true;
}

static void multiarrayPersistFree(void *structure) {
    multiarrayFree(structure);
}

const persistOps persistOpsMultiarray = {
    .type = PERSIST_TYPE_MULTIARRAY,
    .name = "multiarray",
    .snapshot = multiarrayPersistSnapshot,
    .restore = multiarrayPersistRestore,
    .count = multiarrayPersistCount,
    .estimateSize = multiarrayPersistEstimateSize,
    .encodeOp = multiarrayPersistEncodeOp,
    .applyOp = multiarrayPersistApplyOp,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = NULL,
    .free = multiarrayPersistFree,
};

/* ============================================================================
 * Structure-Specific Operations: multilru
 * ============================================================================
 */
#include "../deps/varint/src/varintTagged.h"
#include "multilru.h"

defcleaner(multilru *, multilruFree);

/* Snapshot format (using flex):
 * - Config:
 * [maxLevels][enableWeights][policy][evictStrategy][maxCount][maxWeight]
 * - Entries: For each active entry in handle order:
 * [handle (size_t)][level (uint8_t)][weight (uint64_t, if weights enabled)]
 */

static uint8_t *multilruPersistSnapshot(const void *structure, size_t *len) {
    const multilru *mlru = structure;

    cleaner(flexFree) flex *f = flexNew();

    /* Handle NULL (empty LRU) */
    if (mlru) {
        /* Get stats */
        multilruStats stats;
        multilruGetStats(mlru, &stats);

        /* Check if weights are enabled using the new API */
        bool enableWeights = multilruHasWeights(mlru);

        /* Store configuration: one row */
        flexRow row = FLEX_ROW_INIT;
        flexRowUnsigned(&row, stats.maxLevels);
        flexRowUnsigned(&row, enableWeights ? 1 : 0);
        flexRowUnsigned(&row, 0); /* Policy not exposed in API, use 0 */
        flexRowUnsigned(&row, 0); /* Strategy not exposed, use 0 */
        flexRowUnsigned(&row, stats.maxCount);
        flexRowUnsigned(&row, stats.maxWeight);
        flexRowUnsigned(&row, stats.nextFresh); /* next handle, to keep gaps */
        flexRowAppendExact(&f, &row);

        /* Store entries - iterate through all populated handles, one row
         * each */
        for (size_t handle = 1; handle < stats.nextFresh; handle++) {
            if (multilruIsPopulated(mlru, handle)) {
                flexRowUnsigned(&row, handle);
                flexRowUnsigned(&row, multilruGetLevel(mlru, handle));

                /* ALWAYS store weight if weights are enabled (even if zero) to
                 * maintain consistent field count */
                if (enableWeights) {
                    flexRowUnsigned(&row, multilruGetWeight(mlru, handle));
                }

                flexRowAppendExact(&f, &row);
            }
        }
    }

    *len = flexBytes(f);
    /* @own: caller; free=zfree */
    uint8_t *buf = zmalloc(*len);
    memcpy(buf, f, *len);
    return buf;
}

static void *multilruPersistRestore(const uint8_t *data, size_t len) {
    if (!flexValidateBuffer(data, len)) {
        return NULL;
    }
    /* Restore flex from data */
    cleaner(flexFree) flex *f = zmalloc(len);
    memcpy(f, data, len);

    size_t count = flexCount(f);
    if (count < 6) {
        /* Invalid: need at least config fields */
        return NULL;
    }

    /* Read configuration */
    flexEntry *fe = flexHead(f);
    /* Zeroed: these fields are 64-bit integers, so 'big' must be clear
     * for a malformed wide entry to decode type-only instead of writing
     * through an uninitialised pointer. */
    databox db = {{0}};

    flexGetByType(fe, &db);
    size_t maxLevels = (size_t)db.data.u64;
    fe = flexNext(f, fe);

    flexGetByType(fe, &db);
    bool enableWeights = db.data.u64 != 0;
    fe = flexNext(f, fe);

    flexGetByType(fe, &db);
    /* uint64_t policy = db.data.u64; */ /* Not used currently */
    fe = flexNext(f, fe);

    flexGetByType(fe, &db);
    /* uint64_t evictStrategy = db.data.u64; */ /* Not used currently */
    fe = flexNext(f, fe);

    flexGetByType(fe, &db);
    uint64_t maxCount = db.data.u64;
    fe = flexNext(f, fe);

    flexGetByType(fe, &db);
    uint64_t maxWeight = db.data.u64;
    fe = flexNext(f, fe);

    /* Check if snapshot has nextFresh field (new format) or not (old format) */
    bool hasNextFresh = (count >= 7);
    size_t targetNextFresh = 0;
    size_t configFields = 6;

    if (hasNextFresh) {
        flexGetByType(fe, &db);
        targetNextFresh = (size_t)db.data.u64;
        fe = flexNext(f, fe);
        configFields = 7;
    }

    /* Create multilru with config */
    multilruConfig config = {
        .maxLevels = maxLevels,
        .startCapacity = 0, /* Auto */
        .maxWeight = maxWeight,
        .maxCount = maxCount,
        .policy = MLRU_POLICY_COUNT,     /* Default */
        .evictStrategy = MLRU_EVICT_LRU, /* Default */
        .enableWeights = enableWeights,
    };
    /* @own: caller; free=multilruFree */
    multilru *mlru = multilruNewWithConfig(&config);
    if (!mlru) {
        return NULL;
    }

    /* Disable auto-eviction during restore */
    multilruSetAutoEvict(mlru, false);

    /* Restore entries */
    size_t fieldsPerEntry = enableWeights ? 3 : 2;
    size_t numEntries = (count - configFields) / fieldsPerEntry;

    /* Track which handles are real (from snapshot) vs dummy (gap fillers) */
    cleanerName(zfree, Bools) bool *isRealEntry =
        hasNextFresh && targetNextFresh > 0
            ? zcalloc(targetNextFresh + 1, sizeof(bool))
            : NULL;

    for (size_t i = 0; i < numEntries && fe; i++) {
        /* Read handle */
        flexGetByType(fe, &db);
        size_t targetHandle = (size_t)db.data.u64;
        fe = flexNext(f, fe);
        if (!fe) {
            break;
        }

        /* Read level */
        flexGetByType(fe, &db);
        size_t targetLevel = (size_t)db.data.u64;
        fe = flexNext(f, fe);

        /* Read weight if enabled */
        uint64_t weight = 0;
        if (enableWeights) {
            if (!fe) {
                break;
            }
            flexGetByType(fe, &db);
            weight = db.data.u64;
            fe = flexNext(f, fe);
        }

        /* Insert entries until we reach the target handle value
         * This ensures handle values match after restore
         * Use nextFresh instead of count to handle gaps from deleted entries */
        multilruStats stats;
        multilruGetStats(mlru, &stats);
        while (stats.nextFresh < targetHandle) {
            if (enableWeights) {
                multilruInsertWeighted(mlru, 0);
            } else {
                multilruInsert(mlru);
            }
            multilruGetStats(mlru, &stats);
        }

        /* Insert the actual entry */
        multilruPtr handle;
        if (enableWeights) {
            handle = multilruInsertWeighted(mlru, weight);
        } else {
            handle = multilruInsert(mlru);
        }

        /* Mark this as a real entry */
        if (isRealEntry && handle <= targetNextFresh) {
            isRealEntry[handle] = true;
        }

        /* Promote to target level */
        for (size_t lvl = 0; lvl < targetLevel; lvl++) {
            multilruIncrease(mlru, handle);
        }
    }

    /* If we have nextFresh info, fill to that point and delete dummies to
     * preserve gaps */
    if (hasNextFresh && targetNextFresh > 0 && isRealEntry) {
        /* Fill to nextFresh */
        multilruStats finalStats;
        multilruGetStats(mlru, &finalStats);
        while (finalStats.nextFresh < targetNextFresh) {
            if (enableWeights) {
                multilruInsertWeighted(mlru, 0);
            } else {
                multilruInsert(mlru);
            }
            multilruGetStats(mlru, &finalStats);
        }

        /* Now delete all dummy entries to create gaps */
        for (size_t handle = 1; handle < targetNextFresh; handle++) {
            if (multilruIsPopulated(mlru, handle) && !isRealEntry[handle]) {
                multilruDelete(mlru, handle);
            }
        }
    }

    /* Re-enable auto-eviction */
    multilruSetAutoEvict(mlru, true);

    return mlru;
}

static size_t multilruPersistCount(const void *structure) {
    const multilru *mlru = structure;
    return mlru ? multilruCount(mlru) : 0;
}

static size_t multilruPersistEstimateSize(const void *structure) {
    const multilru *mlru = structure;
    return mlru ? multilruBytes(mlru) : 0;
}

static void multilruPersistFree(void *structure) {
    multilru *mlru = structure;
    if (mlru) {
        multilruFree(mlru);
    }
}

/* WAL operation encoding for multilru:
 * - INSERT: [handle (varint)][weight (varint)]
 * - DELETE: [handle (varint)]
 * - PROMOTE: [handle (varint)]
 * - UPDATE (weight update): [handle (varint)][newWeight (varint)]
 */

static uint8_t *multilruPersistEncodeOp(persistOp op, const void *args,
                                        size_t nargs, size_t *len) {
    if (!args || nargs == 0) {
        *len = 0;
        return NULL;
    }

    /* All multilru ops use handle as first arg
     * args is a pointer to an array of pointers: const void **
     * Cast it properly to access elements */
    const void *const *argsArray = args;
    const multilruPtr *handlePtr = (const multilruPtr *)argsArray[0];
    uint64_t handle = (uint64_t)*handlePtr;

    switch (op) {
    case PERSIST_OP_INSERT: {
        /* INSERT with optional weight: [handle (varint)][weight (varint)] */
        uint64_t weight = 0;
        if (nargs >= 2) {
            const uint64_t *weightPtr = (const uint64_t *)argsArray[1];
            weight = *weightPtr;
        }

        /* Encode handle and weight as self-describing varints */
        /* @own: caller; free=zfree */
        uint8_t *buf = zmalloc(18); /* Max 9 bytes per tagged varint */
        varintWidth handleWidth = varintTaggedPut64(buf, handle);
        varintWidth weightWidth = varintTaggedPut64(buf + handleWidth, weight);
        *len = handleWidth + weightWidth;
        return buf;
    }

    case PERSIST_OP_DELETE:
    case PERSIST_OP_CUSTOM: /* PROMOTE */
    {
        /* DELETE/PROMOTE: [handle (self-describing varint)] */
        /* @own: caller; free=zfree */
        uint8_t *buf = zmalloc(9);
        varintWidth handleWidth = varintTaggedPut64(buf, handle);
        *len = handleWidth;
        return buf;
    }

    case PERSIST_OP_UPDATE: {
        /* UPDATE (weight): [handle (varint)][newWeight (varint)] */
        if (nargs < 2) {
            *len = 0;
            return NULL;
        }
        const uint64_t *weightPtr = (const uint64_t *)argsArray[1];
        uint64_t newWeight = *weightPtr;

        /* @own: caller; free=zfree */
        uint8_t *buf = zmalloc(18); /* Max 9 bytes per tagged varint */
        varintWidth handleWidth = varintTaggedPut64(buf, handle);
        varintWidth weightWidth =
            varintTaggedPut64(buf + handleWidth, newWeight);
        *len = handleWidth + weightWidth;
        return buf;
    }

    default:
        *len = 0;
        return NULL;
    }
}

static bool multilruPersistApplyOp(void *structure, persistOp op,
                                   const uint8_t *data, size_t len) {
    multilru **mlru = structure;

    /* Ensure mlru is initialized */
    if (!*mlru) {
        /* @own: caller; free=multilruFree */
        *mlru = multilruNew();
    }

    /* Disable auto-eviction during WAL replay to prevent infinite loops
     * when inserting dummy entries to fill gaps */
    bool wasAutoEvict = multilruGetAutoEvict(*mlru);
    if (op == PERSIST_OP_INSERT) {
        multilruSetAutoEvict(*mlru, false);
    }

    switch (op) {
    case PERSIST_OP_INSERT: {
        /* Decode: [handle (self-describing varint)][weight (self-describing
         * varint)] */
        if (len < 2) {
            return false;
        }

        /* Decode handle */
        uint64_t handle;
        varintWidth handleWidth = varintTaggedGet64(data, &handle);
        if (handleWidth == 0 || handleWidth > len) {
            return false;
        }

        /* Decode weight */
        size_t offset = handleWidth;
        if (offset >= len) {
            return false;
        }
        uint64_t weight;
        varintWidth weightWidth = varintTaggedGet64(data + offset, &weight);
        if (weightWidth == 0 || offset + weightWidth > len) {
            return false;
        }

        /* Insert dummy entries to fill gaps, then insert the actual entry
         * multilru assigns handles sequentially starting from 1
         * If we're inserting handle 5, we need to ensure handles 1-4 exist
         * first */
        multilruStats stats;
        multilruGetStats(*mlru, &stats);

        /* Insert dummy entries until nextFresh == handle */
        while (stats.nextFresh < handle) {
            multilruInsert(*mlru); /* Insert dummy with no weight */
            multilruGetStats(*mlru, &stats);
        }

        /* Now insert the actual entry with its weight */
        multilruPtr inserted;
        if (weight > 0) {
            inserted = multilruInsertWeighted(*mlru, weight);
        } else {
            inserted = multilruInsert(*mlru);
        }

        /* Verify we got the expected handle */
        if (inserted != (multilruPtr)handle) {
            /* Handle mismatch - this is unexpected */
            multilruSetAutoEvict(*mlru, wasAutoEvict); /* Restore */
            return false;
        }

        /* Restore auto-eviction setting */
        multilruSetAutoEvict(*mlru, wasAutoEvict);
        return true;
    }

    case PERSIST_OP_DELETE: {
        /* Decode: [handle (self-describing varint)] */
        if (len < 1) {
            return false;
        }

        uint64_t handle;
        varintWidth handleWidth = varintTaggedGet64(data, &handle);
        if (handleWidth == 0 || handleWidth > len) {
            return false;
        }

        multilruDelete(*mlru, (multilruPtr)handle);
        return true;
    }

    case PERSIST_OP_CUSTOM: { /* PROMOTE */
        /* Decode: [handle (self-describing varint)] */
        if (len < 1) {
            return false;
        }

        uint64_t handle;
        varintWidth handleWidth = varintTaggedGet64(data, &handle);
        if (handleWidth == 0 || handleWidth > len) {
            return false;
        }

        multilruIncrease(*mlru, (multilruPtr)handle);
        return true;
    }

    case PERSIST_OP_UPDATE: {
        /* Decode: [handle (self-describing varint)][newWeight (self-describing
         * varint)] */
        if (len < 2) {
            return false;
        }

        /* Decode handle */
        uint64_t handle;
        varintWidth handleWidth = varintTaggedGet64(data, &handle);
        if (handleWidth == 0 || handleWidth > len) {
            return false;
        }

        /* Decode new weight */
        size_t offset = handleWidth;
        if (offset >= len) {
            return false;
        }
        uint64_t newWeight;
        varintWidth weightWidth = varintTaggedGet64(data + offset, &newWeight);
        if (weightWidth == 0 || offset + weightWidth > len) {
            return false;
        }

        multilruUpdateWeight(*mlru, (multilruPtr)handle, newWeight);
        return true;
    }

    default:
        return false;
    }
}

/* ============================================================================
 * Structure-Specific Operations: multiOrderedSet
 * ============================================================================
 */
#include "multiOrderedSet.h"

defcleaner(multiOrderedSet *, multiOrderedSetFree);

static uint8_t *multiOrderedSetPersistSnapshot(const void *structure,
                                               size_t *len) {
    const multiOrderedSet *mos = structure;

    /* Create a flex to hold all (score, member) pairs */
    cleaner(flexFree) flex *f = flexNew();

    /* Handle NULL (empty set) */
    if (mos) {
        /* Iterate through all entries in score order */
        mosIterator iter;
        multiOrderedSetIteratorInit(mos, &iter, true); /* forward */

        /* databoxBig, re-armed each step: the iterator writes a 96/128-bit
         * score or member into the armed storage, and a narrow value
         * overwrites the payload pointer. */
        databoxBig member, score;
        for (;;) {
            DATABOX_BIG_INIT(&member);
            DATABOX_BIG_INIT(&score);
            if (!multiOrderedSetIteratorNext(&iter, (databox *)&member,
                                             (databox *)&score)) {
                break;
            }

            /* Store score first, then member (matches sorted order) */
            const databox *pair[2] = {(const databox *)&score,
                                      (const databox *)&member};
            flexAppendMultipleExact(&f, 2, pair);
        }

        multiOrderedSetIteratorRelease(&iter);
    }

    *len = flexBytes(f);
    /* @own: caller; free=zfree */
    uint8_t *buf = zmalloc(*len);
    memcpy(buf, f, *len);
    return buf;
}

static void *multiOrderedSetPersistRestore(const uint8_t *data, size_t len) {
    if (!flexValidateBuffer(data, len)) {
        return NULL;
    }
    /* Restore flex from data */
    cleaner(flexFree) flex *f = zmalloc(len);
    memcpy(f, data, len);

    /* Iterate through (score, member) pairs in the flex */
    size_t count = flexCount(f);
    if (count % 2 != 0) {
        /* Invalid: should have even number of elements (score, member pairs) */
        return NULL;
    }

    /* Create multiOrderedSet (starts as Small tier even if empty) */
    /* @own: caller; free=multiOrderedSetFree */
    multiOrderedSet *mos = multiOrderedSetNew();

    flexEntry *fe = flexHead(f);
    for (size_t i = 0; i < count / 2 && fe; i++) {
        databoxBig score, member; /* room for 96/128-bit scores/members */

        /* Read score */
        flexGetByTypeBig(fe, &score);
        fe = flexNext(f, fe);
        if (!fe) {
            break;
        }

        /* Read member */
        flexGetByTypeBig(fe, &member);
        fe = flexNext(f, fe);

        /* Add to multiOrderedSet */
        multiOrderedSetAdd(&mos, (databox *)&score, (databox *)&member);
    }

    /* Return the multiOrderedSet directly (not wrapped in pointer-to-pointer).
     * The persist system expects restore to return the structure itself, and
     * will pass &structure to applyOp, creating the pointer-to-pointer there.
     */
    return mos;
}

static size_t multiOrderedSetPersistCount(const void *structure) {
    const multiOrderedSet *mos = structure;
    return mos ? multiOrderedSetCount(mos) : 0;
}

static size_t multiOrderedSetPersistEstimateSize(const void *structure) {
    const multiOrderedSet *mos = structure;
    return mos ? multiOrderedSetBytes(mos) : 0;
}

static uint8_t *multiOrderedSetPersistEncodeOp(persistOp op, const void *args,
                                               size_t argc, size_t *len) {
    /* args is array of databox pointers: [score, member] for add/remove */
    const databox *const *boxes = args;

    switch (op) {
    case PERSIST_OP_INSERT: {
        /* Encode: [score][member] */
        if (argc != 2) {
            return NULL;
        }

        size_t totalLen = 0;
        databoxLinear dls[2];

        /* Calculate total length */
        for (size_t i = 0; i < 2; i++) {
            size_t encodedLen;
            const void *encodedVal;
            uint8_t encodedType;
            DATABOX_LINEAR_PARTS_ENCODE(boxes[i], encodedLen, encodedVal,
                                        encodedType, &dls[i]);
            (void)encodedVal;
            (void)encodedType;
            totalLen += 1 + encodedLen; /* type + value */
        }

        *len = totalLen;
        /* @own: caller; free=zfree */
        uint8_t *buf = zmalloc(totalLen);
        size_t offset = 0;

        /* Encode score and member */
        for (size_t i = 0; i < 2; i++) {
            size_t encodedLen;
            const void *encodedVal;
            uint8_t encodedType;
            DATABOX_LINEAR_PARTS_ENCODE(boxes[i], encodedLen, encodedVal,
                                        encodedType, &dls[i]);
            buf[offset++] = encodedType;
            memcpy(buf + offset, encodedVal, encodedLen);
            offset += encodedLen;
        }
        return buf;
    }

    case PERSIST_OP_DELETE: {
        /* Encode: [member] */
        if (argc != 1) {
            return NULL;
        }

        size_t encodedLen;
        const void *encodedVal;
        uint8_t encodedType;
        databoxLinear dl;
        DATABOX_LINEAR_PARTS_ENCODE(boxes[0], encodedLen, encodedVal,
                                    encodedType, &dl);

        *len = 1 + encodedLen;
        /* @own: caller; free=zfree */
        uint8_t *buf = zmalloc(*len);
        buf[0] = encodedType;
        memcpy(buf + 1, encodedVal, encodedLen);
        return buf;
    }

    case PERSIST_OP_CLEAR: {
        *len = 0;
        return NULL;
    }

    default:
        return NULL;
    }
}

static bool multiOrderedSetPersistApplyOp(void *structure, persistOp op,
                                          const uint8_t *data, size_t len) {
    multiOrderedSet **mos = structure;

    /* Ensure mos is initialized (create Small tier if NULL) */
    if (!*mos) {
        /* @own: caller; free=multiOrderedSetFree */
        *mos = multiOrderedSetNew();
    }

    switch (op) {
    case PERSIST_OP_INSERT: {
        /* Decode: [score][member] */
        if (len < 2) {
            return false;
        }

        /* Decode score */
        databox score;
        size_t offset = 0;
        uint8_t scoreType = data[offset++];
        size_t scoreLen = databoxLinearTypeValueLen(scoreType);
        if (scoreLen == 0 || offset + scoreLen > len) {
            return false;
        }
        DATABOX_LINEAR_PARTS_DECODE(scoreType, data + offset, scoreLen, &score);
        offset += scoreLen;

        /* Normalize score type: databoxLinear encodes positive SIGNED_64 as
         * UNSIGNED, so convert back to SIGNED_64 for multiOrderedSet
         * compatibility */
        if (score.type == DATABOX_UNSIGNED_64 &&
            score.data.u64 <= (uint64_t)INT64_MAX) {
            score.type = DATABOX_SIGNED_64;
        }

        /* Decode member */
        if (offset >= len) {
            return false;
        }
        uint8_t memberType = data[offset++];
        size_t memberLen;
        if (DATABOX_LINEAR_TYPE_IS_BYTES(memberType)) {
            memberLen = len - offset;
        } else {
            memberLen = databoxLinearTypeValueLen(memberType);
            if (memberLen == 0 || offset + memberLen > len) {
                return false;
            }
        }

        databox memberTemp;
        DATABOX_LINEAR_PARTS_DECODE(memberType, data + offset, memberLen,
                                    &memberTemp);

        /* CRITICAL: DATABOX_LINEAR_PARTS_DECODE creates a pointer-based BYTES
         * databox that points into the WAL data buffer. We must copy/embed the
         * bytes data because the buffer will be freed after applyOp returns. */
        databox member;
        if (memberTemp.type == DATABOX_BYTES) {
            /* Use databoxNewBytesAllowEmbed to create embedded or allocated
             * copy */
            member = databoxNewBytesAllowEmbed(memberTemp.data.bytes.start,
                                               memberTemp.len);
        } else {
            /* For non-BYTES types (shouldn't happen for member), just copy */
            member = memberTemp;
        }

        multiOrderedSetAdd(mos, &score, &member);
        return true;
    }

    case PERSIST_OP_DELETE: {
        /* Decode: [member] */
        if (len < 1) {
            return false;
        }

        uint8_t memberType = data[0];
        size_t memberLen;
        if (DATABOX_LINEAR_TYPE_IS_BYTES(memberType)) {
            memberLen = len - 1;
        } else {
            memberLen = databoxLinearTypeValueLen(memberType);
            if (memberLen == 0 || 1 + memberLen > len) {
                return false;
            }
        }

        databox memberTemp;
        DATABOX_LINEAR_PARTS_DECODE(memberType, data + 1, memberLen,
                                    &memberTemp);

        /* Copy/embed bytes data (same issue as INSERT) */
        databox member;
        if (memberTemp.type == DATABOX_BYTES) {
            member = databoxNewBytesAllowEmbed(memberTemp.data.bytes.start,
                                               memberTemp.len);
        } else {
            member = memberTemp;
        }

        multiOrderedSetRemove(mos, &member);
        return true;
    }

    case PERSIST_OP_CLEAR: {
        if (*mos) {
            multiOrderedSetReset(*mos);
        }
        return true;
    }

    default:
        return false;
    }
}

static void multiOrderedSetPersistFree(void *structure) {
    multiOrderedSet *mos = structure;
    if (mos) {
        multiOrderedSetFree(mos);
    }
}

const persistOps persistOpsMultiOrderedSet = {
    .type = PERSIST_TYPE_MULTIORDEREDSET,
    .name = "multiOrderedSet",
    .snapshot = multiOrderedSetPersistSnapshot,
    .restore = multiOrderedSetPersistRestore,
    .count = multiOrderedSetPersistCount,
    .estimateSize = multiOrderedSetPersistEstimateSize,
    .encodeOp = multiOrderedSetPersistEncodeOp,
    .applyOp = multiOrderedSetPersistApplyOp,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = NULL,
    .free = multiOrderedSetPersistFree,
};

const persistOps persistOpsMultiLRU = {
    .type = PERSIST_TYPE_MULTILRU,
    .name = "multilru",
    .snapshot = multilruPersistSnapshot,
    .restore = multilruPersistRestore,
    .count = multilruPersistCount,
    .estimateSize = multilruPersistEstimateSize,
    .encodeOp = multilruPersistEncodeOp,
    .applyOp = multilruPersistApplyOp,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = NULL,
    .free = multilruPersistFree,
};

/* ============================================================================
 * Tests
 * ============================================================================
 */
#ifdef DATAKIT_TEST
#include "ctest.h"
#include "dkTestTmp.h"

/* Free a test multidict together with the default class it holds; the
 * class must outlive the dict because teardown runs class callbacks. */
static void testMultidictFreeWithClass(multidict *d) {
    multidictClass *cls = multidictGetClass(d);
    multidictFree(d);
    multidictDefaultClassFree(cls);
}

defcleanerName(multidict *, testMultidictFreeWithClass, Dict);

/* Element 'id' of the multilist wide-element tests, built in 'out' (and
 * 'buf' for a string): 96/128-bit values around each limit and generated
 * ones of both signs, strings, and 64-bit values.  Every value is boxed in
 * the type it reads back as. */
static const databox *persistTestMlValue(uint32_t id, databoxBig *out,
                                         char *buf, size_t cap) {
    const __uint128_t one = 1;
    memset(out, 0, sizeof(*out));
    switch (id % 5) {
    case 0: {
        const __uint128_t positive[] = {one << 64,       (one << 95) - 1,
                                        one << 95,       one << 127,
                                        ~(__uint128_t)0, (one << 127) - 1};
        DATABOX_BIG_UNSIGNED_128(out, positive[(id / 5) % 6]);
        break;
    }
    case 1: {
        const __int128_t negative[] = {-((__int128_t)1 << 63) - 1,
                                       -((__int128_t)1 << 95),
                                       (__int128_t)(one << 127)};
        DATABOX_BIG_SIGNED_128(out, negative[(id / 5) % 3]);
        break;
    }
    case 2: {
        const int n =
            id % 3 == 0
                ? snprintf(buf, cap, "pml-%" PRIu32 "-long-enough-to-copy", id)
                : snprintf(buf, cap, "pml-%" PRIu32, id);
        *(databox *)out = databoxNewBytes(buf, (size_t)n);
        break;
    }
    case 3: {
        /* Unique per id: the low word is the id. */
        const uint64_t hi =
            (((uint64_t)id + 1) * UINT64_C(0x9E3779B97F4A7C15) >> 3) | 1;
        const __uint128_t mag = ((__uint128_t)hi << 64) | id;
        if (id & 4) {
            DATABOX_BIG_SIGNED_128(out, -(__int128_t)mag);
        } else {
            DATABOX_BIG_UNSIGNED_128(out, mag);
        }
        break;
    }
    default:
        *(databox *)out = id & 1 ? databoxNewSigned(-(int64_t)id * 7919 - 1)
                                 : databoxNewUnsigned(UINT64_MAX - id);
        break;
    }

    return (const databox *)out;
}

/* Same type and the same value bytes. */
static bool persistTestBoxSame(const databox *a, const databox *b) {
    if (a->type != b->type) {
        return false;
    }

    switch (a->type) {
    case DATABOX_SIGNED_128:
    case DATABOX_UNSIGNED_128:
        return a->big && a->data.ptr && b->data.ptr &&
               memcmp(a->data.ptr, b->data.ptr, sizeof(__uint128_t)) == 0;
    case DATABOX_BYTES:
        return databoxLen(a) == databoxLen(b) &&
               memcmp(databoxBytes(a), databoxBytes(b), databoxLen(a)) == 0;
    default:
        return a->data.u64 == b->data.u64;
    }
}

/* Read 'ml' through 0xFF-poisoned entries and compare it, in order, with
 * the elements 'ids' names.  Returns the number of errors reported. */
static int persistTestMlVerify(const multilist *ml, const uint32_t *ids,
                               size_t n, const char *what) {
    int err = 0;
    if (!ml || multilistCount(ml) != n) {
        ERR("%s: list holds %zu elements, want %zu", what,
            ml ? multilistCount(ml) : 0, n);
        return err;
    }

    cleaner(mflexStateFree) mflexState *s0 = mflexStateCreate();
    cleaner(mflexStateFree) mflexState *s1 = mflexStateCreate();
    mflexState *s[2] = {s0, s1};
    multilistIterator iter;
    multilistIteratorInitForwardReadOnly((multilist *)ml, s, &iter);
    for (size_t i = 0; i < n; i++) {
        multilistEntry entry;
        memset(&entry, 0xFF, sizeof(entry));
        databoxBig storage;
        char buf[64];
        if (!multilistNext(&iter, &entry) ||
            !persistTestBoxSame(
                &entry.box,
                persistTestMlValue(ids[i], &storage, buf, sizeof(buf)))) {
            ERR("%s: element %zu (id %" PRIu32 ") differs", what, i, ids[i]);
            break;
        }
    }

    multilistIteratorRelease(&iter);
    return err;
}

/* Apply a POP row whose operand is 'operand' (the identity form). */
static bool persistTestMlApplyPop(multilist **ml, bool fromTail,
                                  const databox *operand) {
    cleaner(flexFree) flex *f = flexNewFromDatabox(operand);
    return persistOpsMultilist.applyOp(
        ml, fromTail ? PERSIST_OP_POP_TAIL : PERSIST_OP_POP_HEAD,
        (const uint8_t *)f, flexBytes(f));
}

int persistTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    /* ================================================================
     * Core Framework Tests
     * ================================================================ */

    TEST("create and destroy persist context") {
        persist *p = persistCreate(&persistOpsFlex, NULL);
        if (!p) {
            ERRR("Failed to create persist context");
        }
        persistClose(p);
    }

    TEST("default config values") {
        persistConfig config = persistDefaultConfig();
        if (config.compression != PERSIST_COMPRESS_NONE) {
            ERR("Default compression should be NONE, got %d",
                config.compression);
        }
        if (config.syncMode != PERSIST_SYNC_EVERYSEC) {
            ERR("Default syncMode should be EVERYSEC, got %d", config.syncMode);
        }
        if (!config.autoCompact) {
            ERRR("Default autoCompact should be true");
        }
    }

    /* ================================================================
     * Envelope Round-Trip and Corruption Tests
     * ================================================================
     * Each persist write goes through flexEnvelope. Cover one
     * round-trip per algorithm, then prove that header / payload /
     * digest corruption all fail verify, and that keyed algorithms
     * refuse to snapshot without a key. */

    {
        const persistChecksum algos[] = {
            PERSIST_CHECKSUM_XXH64,       PERSIST_CHECKSUM_XXH3_64,
            PERSIST_CHECKSUM_XXH3_128,    PERSIST_CHECKSUM_BLAKE3_KEYED,
            PERSIST_CHECKSUM_HMAC_SHA256, PERSIST_CHECKSUM_HMAC_BLAKE3,
        };
        const size_t algoCount = sizeof(algos) / sizeof(algos[0]);

        /* 32-byte test key — required size for BLAKE3_KEYED; any size
         * works for HMAC_*; ignored for unkeyed algorithms. */
        static const uint8_t persistTestKey[32] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
            0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
            0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        };

        TEST("envelope snapshot round-trip across all algorithms") {
            for (size_t i = 0; i < algoCount; i++) {
                cleaner(flexFree) flex *f = flexNew();
                flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);
                flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);

                persistConfig config = persistDefaultConfig();
                config.checksumType = algos[i];

                persist *p = persistCreate(&persistOpsFlex, &config);
                persistStore *snapStore = persistStoreMemory(0);
                persistAttachSnapshot(p, snapStore);
                if (PERSIST_CHECKSUM_IS_KEYED(algos[i])) {
                    persistSetIntegrityKey(p, persistTestKey,
                                           sizeof(persistTestKey));
                }

                if (!persistSnapshot(p, f)) {
                    ERR("Snapshot failed for algo %d", (int32_t)algos[i]);
                }
                if (!persistVerifySnapshot(p)) {
                    ERR("Verify failed for algo %d", (int32_t)algos[i]);
                }

                flex *restored = persistRestore(p);
                if (!restored) {
                    ERR("Restore failed for algo %d", (int32_t)algos[i]);
                }
                if (flexCount(restored) != flexCount(f)) {
                    ERR("Restored count mismatch for algo %d",
                        (int32_t)algos[i]);
                }

                flexFree(restored);
                persistClose(p);
            }
        }

        TEST("envelope rejects keyed algorithm without key") {
            persistConfig config = persistDefaultConfig();
            config.checksumType = PERSIST_CHECKSUM_HMAC_SHA256;
            persist *p = persistCreate(&persistOpsFlex, &config);
            persistStore *snapStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);

            cleaner(flexFree) flex *f = flexNew();
            flexPushSigned(&f, 1, FLEX_ENDPOINT_TAIL);
            if (persistSnapshot(p, f)) {
                ERRR("Keyed snapshot without key must be refused");
            }
            persistClose(p);
        }

        TEST("envelope detects header corruption") {
            persistConfig config = persistDefaultConfig();
            config.checksumType = PERSIST_CHECKSUM_XXH3_128;
            persist *p = persistCreate(&persistOpsFlex, &config);
            persistStore *snapStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);

            cleaner(flexFree) flex *f = flexNew();
            flexPushBytes(&f, "corrupt-header-test", 19, FLEX_ENDPOINT_TAIL);
            if (!persistSnapshot(p, f)) {
                ERRR("Snapshot setup failed");
            }

            size_t storeLen = 0;
            const uint8_t *storeBytes =
                persistStoreMemoryBuffer(snapStore, &storeLen);
            if (storeLen < PERSIST_SNAP_HEADER_SIZE + 8) {
                ERR("Snapshot too small: %zu bytes", storeLen);
            }
            /* Flip a byte in the header self-check digest. */
            uint8_t saved = storeBytes[PERSIST_SNAP_HEADER_SIZE - 1];
            ((uint8_t *)storeBytes)[PERSIST_SNAP_HEADER_SIZE - 1] = saved ^ 1u;
            if (persistVerifySnapshot(p)) {
                ERRR("Header-digest corruption must be rejected");
            }
            if (persistRestore(p)) {
                ERRR("Header-digest corruption must reject restore");
            }
            ((uint8_t *)storeBytes)[PERSIST_SNAP_HEADER_SIZE - 1] = saved;
            if (!persistVerifySnapshot(p)) {
                ERRR("Restoring the byte should re-pass verify");
            }

            persistClose(p);
        }

        TEST("envelope detects payload corruption") {
            persistConfig config = persistDefaultConfig();
            config.checksumType = PERSIST_CHECKSUM_HMAC_BLAKE3;
            persist *p = persistCreate(&persistOpsFlex, &config);
            persistStore *snapStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);
            persistSetIntegrityKey(p, persistTestKey, sizeof(persistTestKey));

            cleaner(flexFree) flex *f = flexNew();
            for (uint32_t i = 0; i < 32u; i++) {
                flexPushSigned(&f, (int64_t)i, FLEX_ENDPOINT_TAIL);
            }
            if (!persistSnapshot(p, f)) {
                ERRR("Snapshot setup failed");
            }

            size_t storeLen = 0;
            const uint8_t *storeBytes =
                persistStoreMemoryBuffer(snapStore, &storeLen);
            /* Flip a byte well inside the envelope payload. */
            const size_t target = PERSIST_SNAP_HEADER_SIZE + storeLen / 2u;
            uint8_t saved = storeBytes[target];
            ((uint8_t *)storeBytes)[target] = saved ^ 0x80u;
            if (persistVerifySnapshot(p)) {
                ERRR("Payload corruption must be rejected");
            }
            ((uint8_t *)storeBytes)[target] = saved;
            if (!persistVerifySnapshot(p)) {
                ERRR("Restoring the byte should re-pass verify");
            }

            persistClose(p);
        }

        TEST("envelope detects digest corruption") {
            persistConfig config = persistDefaultConfig();
            config.checksumType = PERSIST_CHECKSUM_XXH64;
            persist *p = persistCreate(&persistOpsFlex, &config);
            persistStore *snapStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);

            cleaner(flexFree) flex *f = flexNew();
            flexPushBytes(&f, "digest-bit-flip", 15, FLEX_ENDPOINT_TAIL);
            if (!persistSnapshot(p, f)) {
                ERRR("Snapshot setup failed");
            }

            size_t storeLen = 0;
            const uint8_t *storeBytes =
                persistStoreMemoryBuffer(snapStore, &storeLen);
            /* Flip the last byte (the envelope's trailing digest). */
            uint8_t saved = storeBytes[storeLen - 1u];
            ((uint8_t *)storeBytes)[storeLen - 1u] = saved ^ 0xffu;
            if (persistVerifySnapshot(p)) {
                ERRR("Digest corruption must be rejected");
            }
            ((uint8_t *)storeBytes)[storeLen - 1u] = saved;
            if (!persistVerifySnapshot(p)) {
                ERRR("Restoring the byte should re-pass verify");
            }

            persistClose(p);
        }

        TEST("persist snapshot rejects tamper at every byte position") {
            /* Exhaustive sweep: every byte in the persisted snapshot
             * (header + envelope-wrapped body) flipped one at a time
             * must cause verify+restore to reject. Catches subtle
             * shifts in the wire format where some byte position
             * isn't actually covered by either the header digest or
             * the envelope's MAC. */
            persistConfig config = persistDefaultConfig();
            config.checksumType = PERSIST_CHECKSUM_HMAC_SHA256;
            persist *p = persistCreate(&persistOpsFlex, &config);
            persistStore *snapStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);
            persistSetIntegrityKey(p, persistTestKey, sizeof(persistTestKey));

            cleaner(flexFree) flex *f = flexNew();
            for (uint32_t i = 0; i < 8u; i++) {
                flexPushSigned(&f, (int64_t)(i * 100u + 1u),
                               FLEX_ENDPOINT_TAIL);
            }
            if (!persistSnapshot(p, f)) {
                ERRR("Baseline snapshot failed");
            }

            size_t storeLen = 0;
            const uint8_t *storeBytes =
                persistStoreMemoryBuffer(snapStore, &storeLen);
            if (storeLen < PERSIST_SNAP_HEADER_SIZE + 16u) {
                ERR("Snapshot unexpectedly small: %zu bytes", storeLen);
            }

            uint32_t accepted_after_tamper = 0;
            for (size_t pos = 0; pos < storeLen; pos++) {
                const uint8_t saved = storeBytes[pos];
                ((uint8_t *)storeBytes)[pos] = saved ^ 0x01u;
                if (persistVerifySnapshot(p)) {
                    accepted_after_tamper++;
                    if (accepted_after_tamper <= 4u) {
                        ERR("Snapshot verify accepted tamper at offset %zu",
                            pos);
                    }
                }
                ((uint8_t *)storeBytes)[pos] = saved;
            }
            if (accepted_after_tamper > 0u) {
                ERR("Total tamper-accept offsets: %u (must be 0)",
                    accepted_after_tamper);
            }
            /* Sanity: the un-flipped snapshot still verifies. */
            if (!persistVerifySnapshot(p)) {
                ERRR("Post-sweep verify must still pass");
            }

            persistClose(p);
        }

        TEST("persist WAL rejects tamper at every byte position of a frame") {
            /* Same sweep for the WAL — one entry, flip every byte of
             * the WAL header AND the entry's length-prefixed envelope,
             * verify each rejection. */
            persistConfig config = persistDefaultConfig();
            config.checksumType = PERSIST_CHECKSUM_HMAC_SHA256;
            persist *p = persistCreate(&persistOpsFlex, &config);
            persistStore *snapStore = persistStoreMemory(0);
            persistStore *walStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);
            persistAttachWAL(p, walStore);
            persistSetIntegrityKey(p, persistTestKey, sizeof(persistTestKey));

            cleaner(flexFree) flex *f = flexNew();
            if (!persistSnapshot(p, f)) {
                ERRR("Initial snapshot failed");
            }
            databox box = {.type = DATABOX_SIGNED_64, .data = {.i = 99}};
            if (!persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box, 1)) {
                ERRR("LogOp failed");
            }
            persistSync(p);

            size_t walLen = 0;
            const uint8_t *walBytes =
                persistStoreMemoryBuffer(walStore, &walLen);
            if (walLen < PERSIST_WAL_HEADER_SIZE + 4u) {
                ERR("WAL unexpectedly small: %zu bytes", walLen);
            }

            uint32_t accepted_after_tamper = 0;
            for (size_t pos = 0; pos < walLen; pos++) {
                const uint8_t saved = walBytes[pos];
                ((uint8_t *)walBytes)[pos] = saved ^ 0x01u;
                if (persistVerifyWAL(p)) {
                    accepted_after_tamper++;
                    if (accepted_after_tamper <= 4u) {
                        ERR("WAL verify accepted tamper at offset %zu", pos);
                    }
                }
                ((uint8_t *)walBytes)[pos] = saved;
            }
            if (accepted_after_tamper > 0u) {
                ERR("Total WAL tamper-accept offsets: %u (must be 0)",
                    accepted_after_tamper);
            }
            if (!persistVerifyWAL(p)) {
                ERRR("Post-sweep WAL verify must still pass");
            }

            persistClose(p);
        }

        TEST("envelope WAL round-trip + replay across keyed algorithm") {
            persistConfig config = persistDefaultConfig();
            config.checksumType = PERSIST_CHECKSUM_HMAC_SHA256;
            persist *p = persistCreate(&persistOpsFlex, &config);
            persistStore *snapStore = persistStoreMemory(0);
            persistStore *walStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);
            persistAttachWAL(p, walStore);
            persistSetIntegrityKey(p, persistTestKey, sizeof(persistTestKey));

            cleaner(flexFree) flex *f = flexNew();
            if (!persistSnapshot(p, f)) {
                ERRR("Initial snapshot failed");
            }

            databox boxes[3] = {
                {.type = DATABOX_SIGNED_64, .data = {.i = 11}},
                {.type = DATABOX_SIGNED_64, .data = {.i = 22}},
                {.type = DATABOX_SIGNED_64, .data = {.i = 33}},
            };
            for (size_t i = 0; i < 3; i++) {
                if (!persistLogOp(p, PERSIST_OP_PUSH_TAIL, &boxes[i], 1)) {
                    ERR("LogOp failed at i=%zu", i);
                }
            }
            persistSync(p);

            if (!persistVerifyWAL(p)) {
                ERRR("WAL verify failed");
            }

            flex *recovered = persistRecover(p);
            if (!recovered) {
                ERRR("Recover returned NULL");
            }
            if (flexCount(recovered) != 3) {
                ERR("Expected 3 entries replayed, got %zu",
                    (size_t)flexCount(recovered));
            }

            flexFree(recovered);
            persistClose(p);
        }

        TEST("persist consumes keys from an hmacKeyring") {
            /* End-to-end integration: keyring owns the key bytes; persist
             * borrows them via hmacKeyringGetRaw. This is the pattern
             * a cluster deployment will use — operators install keys in
             * the keyring, every persist instance binds to a key id and
             * resolves on demand. */
            hmacKeyring *ring = hmacKeyringCreate();
            const uint8_t keyBytes[32] = {
                0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
                0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x01,
                0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0xfe,
            };
            const hmacKeyId currentKeyId = 42u;
            if (!hmacKeyringInstallUnsalted(ring, currentKeyId, &hmacAlgoBLAKE3,
                                            keyBytes, sizeof(keyBytes))) {
                ERRR("Keyring install of current key failed");
            }

            const uint8_t *resolvedKey = NULL;
            size_t resolvedLen = 0;
            if (!hmacKeyringGetRaw(ring, currentKeyId, &resolvedKey,
                                   &resolvedLen, NULL)) {
                ERRR("Keyring lookup of current key failed");
            }

            persistConfig config = persistDefaultConfig();
            config.checksumType = PERSIST_CHECKSUM_HMAC_BLAKE3;
            persist *p = persistCreate(&persistOpsFlex, &config);
            persistStore *snapStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);
            persistSetIntegrityKey(p, resolvedKey, resolvedLen);

            cleaner(flexFree) flex *f = flexNew();
            flexPushBytes(&f, "keyring-end-to-end", 18, FLEX_ENDPOINT_TAIL);
            if (!persistSnapshot(p, f)) {
                ERRR("Snapshot under keyring-supplied key failed");
            }
            if (!persistVerifySnapshot(p)) {
                ERRR("Verify under same key failed");
            }
            flex *restored = persistRestore(p);
            if (!restored || flexCount(restored) != flexCount(f)) {
                ERRR("Restore content mismatch");
            }

            /* Rotation: install a new key, switch persist to the new id,
             * snapshots written under the new key must verify against
             * the new key — and the original snapshot still verifies
             * against the original key. The keyring holds both during
             * the rotation window. */
            const uint8_t newKeyBytes[32] = {
                0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x99, 0x88,
            };
            const hmacKeyId nextKeyId = 43u;
            if (!hmacKeyringInstallUnsalted(ring, nextKeyId, &hmacAlgoBLAKE3,
                                            newKeyBytes, sizeof(newKeyBytes))) {
                ERRR("Keyring install of next key failed");
            }

            /* Persist still bound to the OLD key — verify of existing
             * snapshot passes. */
            if (!persistVerifySnapshot(p)) {
                ERRR("Verify under OLD key after install of NEW must pass");
            }

            /* Switch to NEW key. Verify of existing snapshot now FAILS
             * because the snapshot was MAC'd with the old key. */
            const uint8_t *newResolved = NULL;
            size_t newResolvedLen = 0;
            hmacKeyringGetRaw(ring, nextKeyId, &newResolved, &newResolvedLen,
                              NULL);
            persistSetIntegrityKey(p, newResolved, newResolvedLen);
            if (persistVerifySnapshot(p)) {
                ERRR("Verify with wrong key MUST fail (tamper-evidence)");
            }

            /* Take a NEW snapshot under the new key — should round-trip. */
            if (!persistSnapshot(p, f)) {
                ERRR("Snapshot under new key failed");
            }
            if (!persistVerifySnapshot(p)) {
                ERRR("Verify of new-key snapshot failed");
            }

            /* Retire the old key — keyring drops to one entry. */
            if (!hmacKeyringRetire(ring, currentKeyId)) {
                ERRR("Retire of old key failed");
            }

            flexFree(restored);
            persistClose(p);
            hmacKeyringFree(ring);
        }

        TEST("persistAttachKeyring wires prepared-key for WAL replay") {
            /* persistAttachKeyring binds a persist instance to a
             * (keyring, key_id) pair and pulls BOTH the raw bytes and
             * the prepared handle in one call. WAL replay through
             * flexEnvelopeVerify then uses the prepared fast path on
             * every entry. */
            hmacKeyring *ring = hmacKeyringCreate();
            const uint8_t k[32] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};
            const hmacKeyId kid = 77u;
            if (!hmacKeyringInstallUnsalted(ring, kid, &hmacAlgoSHA256, k,
                                            sizeof(k))) {
                ERRR("Keyring install failed");
            }

            persistConfig config = persistDefaultConfig();
            config.checksumType = PERSIST_CHECKSUM_HMAC_SHA256;
            persist *p = persistCreate(&persistOpsFlex, &config);
            persistStore *snapStore = persistStoreMemory(0);
            persistStore *walStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);
            persistAttachWAL(p, walStore);

            /* persistAttachKeyring replaces persistSetIntegrityKey: it
             * resolves the raw bytes for Wrap and the prepared handle
             * for Verify in a single call. */
            if (!persistAttachKeyring(p, ring, kid)) {
                ERRR("persistAttachKeyring failed");
            }

            cleaner(flexFree) flex *f = flexNew();
            if (!persistSnapshot(p, f)) {
                ERRR("Snapshot under keyring binding failed");
            }
            if (!persistVerifySnapshot(p)) {
                ERRR("Verify under keyring binding failed");
            }

            /* Write several WAL entries — each verify uses the
             * prepared-key fast path automatically. */
            for (uint32_t i = 0; i < 8u; i++) {
                databox box = {.type = DATABOX_SIGNED_64,
                               .data = {.i = (int64_t)i + 1}};
                if (!persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box, 1)) {
                    ERR("LogOp %u failed", i);
                }
            }
            persistSync(p);

            flex *recovered = persistRecover(p);
            if (!recovered || flexCount(recovered) != 8u) {
                ERR("Replay expected 8 entries, got %zu",
                    recovered ? (size_t)flexCount(recovered) : 0);
            }

            /* persistAttachKeyring on a missing id returns false and
             * does not mutate the existing binding. */
            if (persistAttachKeyring(p, ring, 999u)) {
                ERRR("persistAttachKeyring must reject missing key id");
            }
            /* Subsequent verify must still work — the failed re-bind
             * did not clear the existing one. */
            if (!persistVerifySnapshot(p)) {
                ERRR("Failed re-bind must not clear the existing binding");
            }

            flexFree(recovered);
            persistClose(p);
            hmacKeyringFree(ring);
        }
    }

    /* ================================================================
     * Memory Store Tests
     * ================================================================ */

    TEST("memory store write and read") {
        persistStore *store = persistStoreMemory(0);
        if (!store) {
            ERRR("Failed to create memory store");
        }

        const char *testData = "test data 12345";
        ssize_t written = store->write(store->ctx, testData, strlen(testData));
        if (written != (ssize_t)strlen(testData)) {
            ERR("Write returned %zd, expected %zu", written, strlen(testData));
        }

        store->seek(store->ctx, 0, SEEK_SET);

        char buf[32] = {0};
        ssize_t readLen = store->read(store->ctx, buf, sizeof(buf));
        if (readLen != (ssize_t)strlen(testData)) {
            ERR("Read returned %zd, expected %zu", readLen, strlen(testData));
        }

        if (strcmp(buf, testData) != 0) {
            ERR("Read data mismatch: got '%s'", buf);
        }

        store->close(store->ctx);
        zfree(store);
    }

    TEST("memory store seek operations") {
        persistStore *store = persistStoreMemory(0);

        const char *data = "0123456789";
        store->write(store->ctx, data, 10);

        /* SEEK_SET */
        if (store->seek(store->ctx, 5, SEEK_SET) != 5) {
            ERRR("SEEK_SET failed");
        }

        char c;
        store->read(store->ctx, &c, 1);
        if (c != '5') {
            ERR("Expected '5', got '%c'", c);
        }

        /* SEEK_CUR */
        if (store->seek(store->ctx, -3, SEEK_CUR) != 3) {
            ERRR("SEEK_CUR failed");
        }

        /* SEEK_END */
        if (store->seek(store->ctx, -2, SEEK_END) != 8) {
            ERRR("SEEK_END failed");
        }

        store->read(store->ctx, &c, 1);
        if (c != '8') {
            ERR("Expected '8', got '%c'", c);
        }

        store->close(store->ctx);
        zfree(store);
    }

    TEST("memory store truncate") {
        persistStore *store = persistStoreMemory(0);

        store->write(store->ctx, "0123456789", 10);
        if (store->size(store->ctx) != 10) {
            ERRR("Size should be 10");
        }

        store->seek(store->ctx, 5, SEEK_SET);
        store->truncate(store->ctx);

        if (store->size(store->ctx) != 5) {
            ERR("After truncate, size should be 5, got %" PRId64,
                store->size(store->ctx));
        }

        store->close(store->ctx);
        zfree(store);
    }

    /* ================================================================
     * Flex Persistence Tests
     * ================================================================ */

    TEST("flex snapshot and restore") {
        cleaner(flexFree) flex *f = flexNew();
        flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, -100, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);

        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);

        if (!persistSnapshot(p, f)) {
            ERRR("Snapshot failed");
        }

        /* Restore */
        flex *restored = persistRestore(p);
        if (!restored) {
            ERRR("Restore failed");
        }

        if (flexCount(restored) != flexCount(f)) {
            ERR("Count mismatch: %zu vs %zu", flexCount(restored),
                flexCount(f));
        }

        if (flexBytes(restored) != flexBytes(f)) {
            ERR("Size mismatch: %zu vs %zu", flexBytes(restored), flexBytes(f));
        }

        /* Verify contents */
        flexEntry *fe1 = flexHead(f);
        flexEntry *fe2 = flexHead(restored);
        databox box1, box2;

        flexGetByType(fe1, &box1);
        flexGetByType(fe2, &box2);
        if (box1.data.i != box2.data.i || box1.data.i != 42) {
            ERR("First element mismatch: %" PRIdMAX " vs %" PRIdMAX,
                box1.data.i, box2.data.i);
        }

        fe1 = flexNext(f, fe1);
        fe2 = flexNext(restored, fe2);
        flexGetByType(fe1, &box1);
        flexGetByType(fe2, &box2);
        if (box1.data.i != box2.data.i || box1.data.i != -100) {
            ERR("Second element mismatch: %" PRIdMAX " vs %" PRIdMAX,
                box1.data.i, box2.data.i);
        }

        flexFree(restored);
        persistClose(p);
    }

    TEST("flex WAL operations") {
        cleaner(flexFree) flex *f = flexNew();

        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *snapStore = persistStoreMemory(0);
        persistStore *walStore = persistStoreMemory(0);
        persistAttachSnapshot(p, snapStore);
        persistAttachWAL(p, walStore);

        /* Take initial snapshot */
        persistSnapshot(p, f);

        /* Log operations */
        databox box1 = DATABOX_SIGNED(100);
        if (!persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box1, 1)) {
            ERRR("Log PUSH_TAIL failed");
        }

        databox box2 = DATABOX_SIGNED(200);
        if (!persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box2, 1)) {
            ERRR("Log PUSH_TAIL 2 failed");
        }

        databox box3 = DATABOX_SIGNED(-50);
        if (!persistLogOp(p, PERSIST_OP_PUSH_HEAD, &box3, 1)) {
            ERRR("Log PUSH_HEAD failed");
        }

        /* Sync and check stats */
        persistSync(p);
        persistStats stats;
        persistGetStats(p, &stats);
        if (stats.walEntries != 3) {
            ERR("Expected 3 WAL entries, got %" PRIu64, stats.walEntries);
        }

        /* A NULL persist still writes the caller's struct, as zeros. */
        {
            persistStats none;
            memset(&none, 0xA5, sizeof(none));
            persistGetStats(NULL, &none);
            const persistStats zero = {0};
            if (memcmp(&none, &zero, sizeof(none)) != 0) {
                ERRR("persistGetStats(NULL) did not zero the struct");
            }

            persistGetStats(p, NULL); /* must not crash */
        }

        /* Recover and verify */
        flex *recovered = persistRecover(p);
        if (!recovered) {
            ERRR("Recovery failed");
        }

        if (flexCount(recovered) != 3) {
            ERR("Expected 3 elements after recovery, got %zu",
                flexCount(recovered));
        }

        /* Verify order: -50, 100, 200 */
        flexEntry *fe = flexHead(recovered);
        databox box;
        flexGetByType(fe, &box);
        if (box.data.i != -50) {
            ERR("First should be -50, got %" PRIdMAX, box.data.i);
        }

        fe = flexNext(recovered, fe);
        flexGetByType(fe, &box);
        if (box.data.i != 100) {
            ERR("Second should be 100, got %" PRIdMAX, box.data.i);
        }

        fe = flexNext(recovered, fe);
        flexGetByType(fe, &box);
        if (box.data.i != 200) {
            ERR("Third should be 200, got %" PRIdMAX, box.data.i);
        }

        flexFree(recovered);
        persistClose(p);
    }

    TEST("flex compaction") {
        cleaner(flexFree) flex *f = flexNew();
        flexPushSigned(&f, 1, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, 2, FLEX_ENDPOINT_TAIL);

        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *snapStore = persistStoreMemory(0);
        persistStore *walStore = persistStoreMemory(0);
        persistAttachSnapshot(p, snapStore);
        persistAttachWAL(p, walStore);

        /* Initial snapshot */
        persistSnapshot(p, f);

        /* Add via WAL */
        databox box = DATABOX_SIGNED(3);
        persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box, 1);
        flexPushSigned(&f, 3, FLEX_ENDPOINT_TAIL);

        box = DATABOX_SIGNED(4);
        persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box, 1);
        flexPushSigned(&f, 4, FLEX_ENDPOINT_TAIL);

        persistSync(p);

        /* Compact */
        if (!persistCompact(p, f)) {
            ERRR("Compaction failed");
        }

        /* Verify WAL was truncated - new WAL should be small */
        int64_t walSize = walStore->size(walStore->ctx);
        if (walSize > 100) { /* Header only should be ~24 bytes */
            ERR("WAL should be small after compaction, got %" PRId64, walSize);
        }

        /* Recover should still work */
        flex *recovered = persistRecover(p);
        if (!recovered) {
            ERRR("Recovery after compaction failed");
        }

        if (flexCount(recovered) != 4) {
            ERR("Expected 4 elements, got %zu", flexCount(recovered));
        }

        flexFree(recovered);
        persistClose(p);
    }

    /* ================================================================
     * Intset Persistence Tests
     * ================================================================ */

    TEST("intset snapshot and restore") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;
        intsetAdd(&is, 42, &success);
        intsetAdd(&is, -100, &success);
        intsetAdd(&is, 999999, &success);

        persist *p = persistCreate(&persistOpsIntset, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);

        if (!persistSnapshot(p, is)) {
            ERRR("Intset snapshot failed");
        }

        intset *restored = persistRestore(p);
        if (!restored) {
            ERRR("Intset restore failed");
        }

        if (intsetCount(restored) != intsetCount(is)) {
            ERR("Count mismatch: %zu vs %zu", intsetCount(restored),
                intsetCount(is));
        }

        if (!intsetFind(restored, 42)) {
            ERRR("42 not found");
        }
        if (!intsetFind(restored, -100)) {
            ERRR("-100 not found");
        }
        if (!intsetFind(restored, 999999)) {
            ERRR("999999 not found");
        }

        intsetFree(restored);
        persistClose(p);
    }

    TEST("intset WAL operations") {
        cleaner(intsetFree) intset *is = intsetNew();

        persist *p = persistCreate(&persistOpsIntset, NULL);
        persistStore *snapStore = persistStoreMemory(0);
        persistStore *walStore = persistStoreMemory(0);
        persistAttachSnapshot(p, snapStore);
        persistAttachWAL(p, walStore);

        persistSnapshot(p, is);

        /* Log insert operations */
        int64_t v1 = 100;
        persistLogOp(p, PERSIST_OP_INSERT, &v1, 1);
        int64_t v2 = 200;
        persistLogOp(p, PERSIST_OP_INSERT, &v2, 1);
        int64_t v3 = 150;
        persistLogOp(p, PERSIST_OP_INSERT, &v3, 1);

        persistSync(p);

        /* Recover */
        intset *recovered = persistRecover(p);
        if (!recovered) {
            ERRR("Intset recovery failed");
        }

        if (intsetCount(recovered) != 3) {
            ERR("Expected 3 elements, got %zu", intsetCount(recovered));
        }

        if (!intsetFind(recovered, 100) || !intsetFind(recovered, 150) ||
            !intsetFind(recovered, 200)) {
            ERRR("Missing values after recovery");
        }

        intsetFree(recovered);
        persistClose(p);
    }

    /* ================================================================
     * Multimap Persistence Tests
     * ================================================================ */

    TEST("multimap snapshot and restore") {
        cleaner(multimapFree) multimap *m = multimapNew(2); /* key + value */

        databox key1 = DATABOX_WITH_BYTES((uint8_t *)"key1", 4);
        databox val1 = DATABOX_SIGNED(100);
        const databox *entry1[] = {&key1, &val1};
        multimapInsert(&m, entry1);

        databox key2 = DATABOX_WITH_BYTES((uint8_t *)"key2", 4);
        databox val2 = DATABOX_SIGNED(200);
        const databox *entry2[] = {&key2, &val2};
        multimapInsert(&m, entry2);

        persist *p = persistCreate(&persistOpsMultimap, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);

        if (!persistSnapshot(p, m)) {
            ERRR("Multimap snapshot failed");
        }

        multimap *restored = persistRestore(p);
        if (!restored) {
            ERRR("Multimap restore failed");
        }

        if (multimapCount(restored) != multimapCount(m)) {
            ERR("Count mismatch: %zu vs %zu", multimapCount(restored),
                multimapCount(m));
        }

        /* Verify entries */
        databox foundVal;
        databox *foundPtrs[] = {&foundVal};
        if (!multimapLookup(restored, &key1, foundPtrs)) {
            ERRR("key1 not found");
        }
        if (foundVal.data.i != 100) {
            ERR("key1 value should be 100, got %" PRIdMAX, foundVal.data.i);
        }

        if (!multimapLookup(restored, &key2, foundPtrs)) {
            ERRR("key2 not found");
        }
        if (foundVal.data.i != 200) {
            ERR("key2 value should be 200, got %" PRIdMAX, foundVal.data.i);
        }

        multimapFree(restored);
        persistClose(p);
    }

    /* ================================================================
     * Verification Tests
     * ================================================================ */

    TEST("verify snapshot integrity") {
        cleaner(flexFree) flex *f = flexNew();
        flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);

        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, f);

        if (!persistVerifySnapshot(p)) {
            ERRR("Valid snapshot should verify");
        }

        /* Corrupt the data */
        size_t len;
        const uint8_t *buf = persistStoreMemoryBuffer(store, &len);
        if (len > 40) {
            /* Modify a byte in the body */
            ((uint8_t *)buf)[40] ^= 0xFFu;
        }

        if (persistVerifySnapshot(p)) {
            ERRR("Corrupted snapshot should fail verification");
        }

        persistClose(p);
    }

    TEST("type and op name lookups") {
        if (strcmp(persistTypeName(PERSIST_TYPE_FLEX), "flex") != 0) {
            ERRR("Wrong name for FLEX type");
        }
        if (strcmp(persistTypeName(PERSIST_TYPE_INTSET), "intset") != 0) {
            ERRR("Wrong name for INTSET type");
        }
        if (strcmp(persistOpName(PERSIST_OP_INSERT), "INSERT") != 0) {
            ERRR("Wrong name for INSERT op");
        }
        if (strcmp(persistOpName(PERSIST_OP_PUSH_TAIL), "PUSH_TAIL") != 0) {
            ERRR("Wrong name for PUSH_TAIL op");
        }
    }

    /* ================================================================
     * Round-Trip Consistency Tests
     * ================================================================ */

    TEST("flex round-trip with many elements") {
        cleaner(flexFree) flex *f = flexNew();

        /* Add various types */
        for (int i = 0; i < 100; i++) {
            flexPushSigned(&f, i * 11 - 500, FLEX_ENDPOINT_TAIL);
        }
        flexPushBytes(&f, "test string", 11, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, UINT64_MAX, FLEX_ENDPOINT_TAIL);

        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, f);

        flex *restored = persistRestore(p);
        if (!restored) {
            ERRR("Restore failed");
        }

        /* Byte-for-byte comparison */
        if (flexBytes(f) != flexBytes(restored)) {
            ERRR("Size mismatch");
        }
        if (memcmp(f, restored, flexBytes(f)) != 0) {
            ERRR("Content mismatch");
        }

        flexFree(restored);
        persistClose(p);
    }

    TEST("intset round-trip with edge cases") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        /* Add edge case values */
        intsetAdd(&is, 0, &success);
        intsetAdd(&is, INT64_MAX, &success);
        intsetAdd(&is, INT64_MIN, &success);
        intsetAdd(&is, -1, &success);
        intsetAdd(&is, 1, &success);
        intsetAdd(&is, INT16_MAX, &success);
        intsetAdd(&is, INT16_MIN, &success);
        intsetAdd(&is, INT32_MAX, &success);
        intsetAdd(&is, INT32_MIN, &success);

        persist *p = persistCreate(&persistOpsIntset, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, is);

        intset *restored = persistRestore(p);
        if (!restored) {
            ERRR("Restore failed");
        }

        /* Verify all values */
        int64_t values[] = {0,         INT64_MAX, INT64_MIN, -1,       1,
                            INT16_MAX, INT16_MIN, INT32_MAX, INT32_MIN};
        for (size_t i = 0; i < COUNT_ARRAY(values); i++) {
            if (!intsetFind(restored, values[i])) {
                ERR("Missing value: %" PRId64, values[i]);
            }
        }

        intsetFree(restored);
        persistClose(p);
    }

    /* ================================================================
     * Multilist Persistence Tests
     * ================================================================ */

    TEST("128-bit values survive multimap snapshot and multilist restore") {
        /* Restore decodes every member; a 96/128-bit member must land in
         * storage the decode box owns. */
        const __int128_t wide = -((__int128_t)1 << 100) - 7;
        databoxBig wideBox;
        DATABOX_BIG_SIGNED_128(&wideBox, wide);

        cleaner(multimapFree) multimap *m = multimapNew(2);
        const databox key = databoxNewBytesString("wide");
        const databox *row[] = {&key, (const databox *)&wideBox};
        multimapInsert(&m, row);
        persist *pm = persistCreate(&persistOpsMultimap, NULL);
        persistAttachSnapshot(pm, persistStoreMemory(0));
        if (!persistSnapshot(pm, m)) {
            ERRR("multimap snapshot with a 128-bit value failed");
        }
        cleaner(multimapFree) multimap *mr = persistRestore(pm);
        databoxBig got;
        DATABOX_BIG_INIT(&got);
        databox *gotPtrs[] = {(databox *)&got};
        if (!mr || !multimapLookup(mr, &key, gotPtrs)) {
            ERRR("restored multimap lost the 128-bit row");
        } else if (got.type != DATABOX_SIGNED_128 || *got.data.i128 != wide) {
            ERRR("restored multimap 128-bit value read back wrong");
        }
        persistClose(pm);

        /* multilist restore decodes from the snapshot flex directly, so
         * hand it a flex carrying the 128-bit element. */
        cleaner(flexFree) flex *mlSnap = flexNew();
        flexPushByType(&mlSnap, (const databox *)&wideBox, FLEX_ENDPOINT_TAIL);
        cleaner(multilistFree) multilist *mlr = persistOpsMultilist.restore(
            (const uint8_t *)mlSnap, flexBytes(mlSnap));
        if (!mlr || multilistCount(mlr) != 1) {
            ERRR("restored multilist lost the 128-bit element");
        }
    }

    TEST("multilist snapshot and restore") {
        cleaner(multilistFree) multilist *ml =
            multilistNew(FLEX_CAP_LEVEL_2048, 0);
        mflexState *state = NULL;

        /* Add various elements */
        for (int i = 0; i < 50; i++) {
            databox box = DATABOX_SIGNED(i * 10);
            multilistPushByTypeTail(&ml, state, &box);
        }

        persist *p = persistCreate(&persistOpsMultilist, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);

        if (!persistSnapshot(p, ml)) {
            ERRR("Multilist snapshot failed");
        }

        multilist *restored = persistRestore(p);
        if (!restored) {
            ERRR("Multilist restore failed");
        }

        if (multilistCount(restored) != multilistCount(ml)) {
            ERR("Count mismatch: %zu vs %zu", multilistCount(restored),
                multilistCount(ml));
        }

        /* Verify first and last values */
        multilistEntry entry;
        mflexState *state2 = NULL;
        if (multilistIndex(restored, state2, 0, &entry, true)) {
            if (entry.box.data.i != 0) {
                ERR("First element should be 0, got %" PRIdMAX,
                    entry.box.data.i);
            }
        } else {
            ERRR("Could not get first element");
        }

        if (multilistIndex(restored, state2, -1, &entry, true)) {
            if (entry.box.data.i != 490) {
                ERR("Last element should be 490, got %" PRIdMAX,
                    entry.box.data.i);
            }
        } else {
            ERRR("Could not get last element");
        }

        multilistFree(restored);
        persistClose(p);
    }

    TEST("multilist WAL operations") {
        cleaner(multilistFree) multilist *ml =
            multilistNew(FLEX_CAP_LEVEL_2048, 0);
        mflexState *state = NULL;

        persist *p = persistCreate(&persistOpsMultilist, NULL);
        persistStore *snapStore = persistStoreMemory(0);
        persistStore *walStore = persistStoreMemory(0);
        persistAttachSnapshot(p, snapStore);
        persistAttachWAL(p, walStore);

        persistSnapshot(p, ml);

        /* Log push operations */
        for (int i = 1; i <= 5; i++) {
            databox box = DATABOX_SIGNED(i * 100);
            multilistPushByTypeTail(&ml, state, &box);
            persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box, 1);
        }

        persistSync(p);

        /* Recover */
        multilist *recovered = persistRecover(p);
        if (!recovered) {
            ERRR("Multilist recovery failed");
        }

        if (multilistCount(recovered) != 5) {
            ERR("Expected 5 elements, got %zu", multilistCount(recovered));
        }

        multilistFree(recovered);
        persistClose(p);
    }

    TEST("multilist: 96/128-bit elements survive snapshot and restore") {
        /* One list per tier: Small, Medium (between the 512-byte limit and
         * 3x it), and Full with compressed interior nodes. */
        const struct {
            const char *name;
            flexCapSizeLimit limit;
            uint32_t depth;
            uint32_t count;
            size_t minBytes;
            size_t maxBytes;
        } configs[] = {
            {"Small", FLEX_CAP_LEVEL_65536, 0, 40, 0, 65536},
            {"Medium", FLEX_CAP_LEVEL_512, 0, 50, 512, 3 * 512},
            {"Full", FLEX_CAP_LEVEL_128, 1, 400, 3 * 128, SIZE_MAX},
        };

        for (size_t c = 0; c < sizeof(configs) / sizeof(*configs); c++) {
            const char *const name = configs[c].name;
            uint32_t ids[400];
            size_t n = 0;
            cleaner(multilistFree) multilist *ml =
                multilistNew(configs[c].limit, configs[c].depth);
            cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
            for (uint32_t id = 0; id < configs[c].count; id++) {
                databoxBig storage;
                char buf[64];
                const databox *box =
                    persistTestMlValue(id, &storage, buf, sizeof(buf));
                if (id % 6 == 5) {
                    multilistPushByTypeHead(&ml, state, box);
                    memmove(&ids[1], &ids[0], n * sizeof(*ids));
                    ids[0] = id;
                } else {
                    multilistPushByTypeTail(&ml, state, box);
                    ids[n] = id;
                }
                n++;
            }

            const size_t bytes = multilistBytes(ml);
            if (bytes <= configs[c].minBytes || bytes > configs[c].maxBytes) {
                ERR("%s: %zu bytes is outside its tier's range", name, bytes);
            }

            persist *p = persistCreate(&persistOpsMultilist, NULL);
            persistAttachSnapshot(p, persistStoreMemory(0));
            if (!persistSnapshot(p, ml)) {
                ERR("%s: snapshot failed", name);
            }

            cleaner(multilistFree) multilist *restored = persistRestore(p);
            err += persistTestMlVerify(restored, ids, n, name);

            /* The snapshot is the list's dump, byte for byte. */
            size_t len = 0;
            cleanerName(zfree, Bytes) uint8_t *snap =
                persistOpsMultilist.snapshot(ml, &len);
            cleaner(flexFree) flex *dump = multilistDump(ml, state);
            if (!snap || len != flexBytes(dump) || memcmp(snap, dump, len)) {
                ERR("%s: snapshot bytes differ from the dump", name);
            }

            persistClose(p);
        }
    }

    TEST("multilist restore: deep-validates the snapshot before decoding") {
        /* A header that claims one entry more than the body holds passes
         * the O(1) envelope check; decoding it would walk off the end. */
        cleaner(flexFree) flex *f = flexNew();
        for (int i = 1; i <= 4; i++) {
            flexPushUnsigned(&f, (uint64_t)i, FLEX_ENDPOINT_TAIL);
        }

        const size_t bytes = flexBytes(f);
        cleanerName(zfree, Bytes) uint8_t *tampered = zmalloc(bytes);
        memcpy(tampered, f, bytes);
        /* A small flex has a 1-byte total and a 1-byte count. */
        if (tampered[1] != 4) {
            ERRR("unexpected flex header layout");
        } else {
            tampered[1] = 5;
            if (!flexValidateBuffer(tampered, bytes)) {
                ERRR("the envelope check should accept the tampered count");
            }

            cleaner(multilistFree) multilist *restored =
                persistOpsMultilist.restore(tampered, bytes);
            if (restored) {
                ERRR("restore accepted a snapshot whose count lies");
            }
        }
    }

    TEST("multilist WAL: 96/128-bit operands replay under strictRecovery") {
        /* strictRecovery fails recovery on any row apply refuses, so a
         * dropped PUSH or REPLACE operand fails the test. */
        persistConfig cfg = persistDefaultConfig();
        cfg.strictRecovery = true;
        persist *p = persistCreate(&persistOpsMultilist, &cfg);
        persistAttachSnapshot(p, persistStoreMemory(0));
        persistAttachWAL(p, persistStoreMemory(0));

        cleaner(multilistFree) multilist *ml =
            multilistNew(FLEX_CAP_LEVEL_128, 0);
        cleaner(mflexStateFree) mflexState *s0 = mflexStateCreate();
        cleaner(mflexStateFree) mflexState *s1 = mflexStateCreate();
        mflexState *s[2] = {s0, s1};
        persistSnapshot(p, ml);

        uint32_t ids[200];
        size_t n = 0;
        uint32_t nextId = 0;
        databoxBig storage;
        char buf[64];

        /* PUSH_HEAD / PUSH_TAIL */
        for (int i = 0; i < 120; i++) {
            const uint32_t id = nextId++;
            const databox *box =
                persistTestMlValue(id, &storage, buf, sizeof(buf));
            const bool head = i % 4 == 3;
            if (head) {
                multilistPushByTypeHead(&ml, s0, box);
                memmove(&ids[1], &ids[0], n * sizeof(*ids));
                ids[0] = id;
            } else {
                multilistPushByTypeTail(&ml, s0, box);
                ids[n] = id;
            }
            n++;
            persistLogOp(p, head ? PERSIST_OP_PUSH_HEAD : PERSIST_OP_PUSH_TAIL,
                         box, 1);
        }

        /* REPLACE [index, wide value] */
        const size_t replaceAt[] = {0, 5, n - 1};
        for (size_t k = 0; k < sizeof(replaceAt) / sizeof(*replaceAt); k++) {
            const uint32_t id = 5 * (nextId++) + (uint32_t)(k % 2);
            const databox *box =
                persistTestMlValue(id, &storage, buf, sizeof(buf));
            const databox idxBox = DATABOX_SIGNED((int64_t)replaceAt[k]);
            const databox *args[2] = {&idxBox, box};
            multilistReplaceByTypeAtIndex(&ml, s0, replaceAt[k], box);
            persistLogOp(p, PERSIST_OP_REPLACE, args, 2);
            ids[replaceAt[k]] = id;
        }

        /* ROTATE, starting with a wide tail */
        {
            const uint32_t id = 5 * (nextId++) + 1;
            const databox *box =
                persistTestMlValue(id, &storage, buf, sizeof(buf));
            multilistPushByTypeTail(&ml, s0, box);
            persistLogOp(p, PERSIST_OP_PUSH_TAIL, box, 1);
            ids[n++] = id;
        }

        for (int r = 0; r < 3; r++) {
            const uint32_t moved = ids[n - 1];
            multilistRotate(ml, s);
            persistLogOp(p, PERSIST_OP_ROTATE, NULL, 0);
            memmove(&ids[1], &ids[0], (n - 1) * sizeof(*ids));
            ids[0] = moved;
        }

        /* Operand-less POPs over wide values and strings */
        for (int k = 0; k < 7; k++) {
            const bool fromTail = k % 2 == 0;
            databox got = {{0}};
            multilistPop(&ml, s0, &got, fromTail);
            databoxFreeData(&got);
            persistLogOp(p,
                         fromTail ? PERSIST_OP_POP_TAIL : PERSIST_OP_POP_HEAD,
                         NULL, 0);
            if (!fromTail) {
                memmove(&ids[0], &ids[1], (n - 1) * sizeof(*ids));
            }
            n--;
        }

        err += persistTestMlVerify(ml, ids, n, "live list");
        if (multilistBytes(ml) <= 3 * 128) {
            ERRR("the WAL list never reached the Full tier");
        }

        persistSync(p);
        cleaner(multilistFree) multilist *recovered = persistRecover(p);
        persistStats stats;
        persistGetStats(p, &stats);
        if (!recovered) {
            ERRR("strict recovery refused the wide WAL");
        } else if (stats.entriesSkipped != 0) {
            ERR("recovery skipped %" PRIu64 " rows", stats.entriesSkipped);
        }

        err += persistTestMlVerify(recovered, ids, n, "recovered list");
        persistClose(p);
    }

    TEST("multilist WAL: POP identity with 96/128-bit operands") {
        /* Ids 0..59 in order: 0, 1 and 3 mod 5 are wide (3 is unique),
         * 2 a string, 4 a unique 64-bit value.  The replay state stands in
         * for persistRecover's, which the Full tier needs. */
        cleaner(multilistFree) multilist *ml =
            multilistNew(FLEX_CAP_LEVEL_128, 0);
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        persistSetReplayState(state);
        uint32_t ids[60];
        size_t n = 0;
        databoxBig storage;
        char buf[64];
        for (uint32_t id = 0; id < 60; id++) {
            multilistPushByTypeTail(
                &ml, state, persistTestMlValue(id, &storage, buf, sizeof(buf)));
            ids[n++] = id;
        }

        const struct {
            const char *what;
            bool fromTail;
            uint32_t operandId;
            int deep;   /* expected change in the deep-remove counter */
            int absent; /* expected change in the absent counter */
        } pops[] = {
            /* the popping end matches: a plain pop */
            {"wide operand at the head", false, 0, 0, 0},
            /* present elsewhere: removed by identity, searched from the
             * tail */
            {"wide operand mid-list", true, 33, 1, 0},
            /* absent: an already-applied pop */
            {"absent wide operand", false, 1003, 0, 1},
            /* a 64-bit operand found by scanning past wide elements */
            {"64-bit operand among wide elements", false, 19, 1, 0},
            {"64-bit operand at the tail", true, 59, 0, 0},
        };

        for (size_t k = 0; k < sizeof(pops) / sizeof(*pops); k++) {
            const uint64_t deep0 = persistMultilistPopIdentityDeepRemoveTotal();
            const uint64_t absent0 =
                persistMultilistPopIdentityAbsentNoopTotal();
            const databox *operand = persistTestMlValue(
                pops[k].operandId, &storage, buf, sizeof(buf));
            if (!persistTestMlApplyPop(&ml, pops[k].fromTail, operand)) {
                ERR("%s: apply refused the row", pops[k].what);
            }

            const uint64_t deep =
                persistMultilistPopIdentityDeepRemoveTotal() - deep0;
            const uint64_t absent =
                persistMultilistPopIdentityAbsentNoopTotal() - absent0;
            if (deep != (uint64_t)pops[k].deep ||
                absent != (uint64_t)pops[k].absent) {
                ERR("%s: ran the wrong arm (deep %" PRIu64 ", absent %" PRIu64
                    ")",
                    pops[k].what, deep, absent);
            }

            for (size_t i = 0; i < n; i++) {
                if (ids[i] == pops[k].operandId) {
                    memmove(&ids[i], &ids[i + 1], (n - i - 1) * sizeof(*ids));
                    n--;
                    break;
                }
            }

            err += persistTestMlVerify(ml, ids, n, pops[k].what);
        }

        /* A REPLACE whose index is not a 64-bit integer is refused. */
        {
            databoxBig wideIdx;
            DATABOX_BIG_UNSIGNED_128(&wideIdx, (__uint128_t)1 << 64);
            const databox value = DATABOX_SIGNED(7);
            const databox *args[2] = {(const databox *)&wideIdx, &value};
            size_t len = 0;
            cleanerName(zfree, Bytes) uint8_t *row =
                persistOpsMultilist.encodeOp(PERSIST_OP_REPLACE, args, 2, &len);
            if (!row || persistOpsMultilist.applyOp(&ml, PERSIST_OP_REPLACE,
                                                    row, len)) {
                ERRR("REPLACE with a 128-bit index was not refused");
            }

            err += persistTestMlVerify(ml, ids, n, "after refused REPLACE");
        }

        persistSetReplayState(NULL);
    }

    TEST("multilist WAL: refused DELETE_AT and REPLACE replay as no-ops") {
        /* Older writers logged range deletes and replaces that the list
         * refused.  Replay sees the same state, so it refuses them too;
         * the record must apply as a no-op, not stop recovery.  Run at
         * three fill limits, from one node to many. */
        const flexCapSizeLimit limits[] = {
            FLEX_CAP_LEVEL_2048, FLEX_CAP_LEVEL_512, FLEX_CAP_LEVEL_64};
        for (size_t t = 0; t < COUNT_ARRAY(limits); t++) {
            cleaner(multilistFree) multilist *ml = multilistNew(limits[t], 0);
            cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
            persistSetReplayState(state);
            uint32_t ids[40];
            size_t n = 0;
            databoxBig storage;
            char buf[64];
            for (uint32_t id = 0; id < 40; id++) {
                multilistPushByTypeTail(
                    &ml, state,
                    persistTestMlValue(id, &storage, buf, sizeof(buf)));
                ids[n++] = id;
            }

            const struct {
                const char *what;
                persistOp op;
                int64_t a;
                int64_t b;
            } noops[] = {
                {"zero-count DELETE_AT", PERSIST_OP_DELETE_AT, 3, 0},
                {"negative-count DELETE_AT", PERSIST_OP_DELETE_AT, 3, -2},
                {"DELETE_AT past the tail", PERSIST_OP_DELETE_AT, 40, 2},
                {"DELETE_AT past the head", PERSIST_OP_DELETE_AT, -41, 2},
                {"REPLACE past the tail", PERSIST_OP_REPLACE, 40, 7},
                {"REPLACE past the head", PERSIST_OP_REPLACE, -41, 7},
            };
            for (size_t k = 0; k < COUNT_ARRAY(noops); k++) {
                const databox a = DATABOX_SIGNED(noops[k].a);
                const databox b = DATABOX_SIGNED(noops[k].b);
                const databox *args[2] = {&a, &b};
                size_t len = 0;
                cleanerName(zfree, Bytes) uint8_t *row =
                    persistOpsMultilist.encodeOp(noops[k].op, args, 2, &len);
                if (!row ||
                    !persistOpsMultilist.applyOp(&ml, noops[k].op, row, len)) {
                    ERR("tier %zu: %s did not replay as a no-op", t,
                        noops[k].what);
                }

                err += persistTestMlVerify(ml, ids, n, noops[k].what);
            }

            /* A real range delete still applies. */
            {
                const databox a = DATABOX_SIGNED(5);
                const databox b = DATABOX_SIGNED(3);
                const databox *args[2] = {&a, &b};
                size_t len = 0;
                cleanerName(zfree, Bytes) uint8_t *row =
                    persistOpsMultilist.encodeOp(PERSIST_OP_DELETE_AT, args, 2,
                                                 &len);
                if (!row || !persistOpsMultilist.applyOp(
                                &ml, PERSIST_OP_DELETE_AT, row, len)) {
                    ERR("tier %zu: DELETE_AT [5, 3] did not apply", t);
                }

                memmove(&ids[5], &ids[8], (n - 8) * sizeof(*ids));
                n -= 3;
                err +=
                    persistTestMlVerify(ml, ids, n, "after DELETE_AT [5, 3]");
            }

            /* A DELETE_AT whose operands are not integers is malformed. */
            {
                const databox a = databoxNewBytesString("five");
                const databox b = DATABOX_SIGNED(1);
                const databox *args[2] = {&a, &b};
                size_t len = 0;
                cleanerName(zfree, Bytes) uint8_t *row =
                    persistOpsMultilist.encodeOp(PERSIST_OP_DELETE_AT, args, 2,
                                                 &len);
                if (!row || persistOpsMultilist.applyOp(
                                &ml, PERSIST_OP_DELETE_AT, row, len)) {
                    ERR("tier %zu: a string DELETE_AT start was not refused",
                        t);
                }

                err += persistTestMlVerify(ml, ids, n, "after malformed op");
            }

            /* An unsigned operand above INT64_MAX is malformed, never a
             * negative index: UINT64_MAX would otherwise address the
             * tail. */
            {
                const databox huge = DATABOX_UNSIGNED(UINT64_MAX);
                const databox one = DATABOX_SIGNED(1);
                const databox value = DATABOX_SIGNED(7);
                const struct {
                    const char *what;
                    persistOp op;
                    const databox *a;
                    const databox *b;
                } bad[] = {
                    {"REPLACE at UINT64_MAX", PERSIST_OP_REPLACE, &huge,
                     &value},
                    {"DELETE_AT from UINT64_MAX", PERSIST_OP_DELETE_AT, &huge,
                     &one},
                    {"DELETE_AT of UINT64_MAX", PERSIST_OP_DELETE_AT, &one,
                     &huge},
                };
                for (size_t k = 0; k < COUNT_ARRAY(bad); k++) {
                    const databox *args[2] = {bad[k].a, bad[k].b};
                    size_t len = 0;
                    cleanerName(zfree, Bytes) uint8_t *row =
                        persistOpsMultilist.encodeOp(bad[k].op, args, 2, &len);
                    if (!row ||
                        persistOpsMultilist.applyOp(&ml, bad[k].op, row, len)) {
                        ERR("tier %zu: %s was not refused", t, bad[k].what);
                    }

                    err += persistTestMlVerify(ml, ids, n, bad[k].what);
                }
            }

            persistSetReplayState(NULL);
        }
    }

    /* ================================================================
     * Multidict Persistence Tests
     * ================================================================ */

    TEST("multidict snapshot and restore") {
        /* @own: container; free=multidictDefaultClassFree */
        multidictClass *qdc = multidictDefaultClassNew();
        cleanerName(testMultidictFreeWithClass, Dict) multidict *d =
            multidictNew(&multidictTypeExactKey, qdc, 0);

        /* Add key-value pairs */
        for (int i = 0; i < 20; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 1000);
            multidictAdd(d, &key, &val);
        }

        persist *p = persistCreate(&persistOpsMultidict, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);

        if (!persistSnapshot(p, d)) {
            ERRR("Multidict snapshot failed");
        }

        multidict *restored = persistRestore(p);
        if (!restored) {
            ERRR("Multidict restore failed");
        }

        if (multidictCount(restored) != multidictCount(d)) {
            ERR("Count mismatch: %" PRIu64 " vs %" PRIu64,
                multidictCount(restored), multidictCount(d));
        }

        /* Verify some entries */
        for (int i = 0; i < 20; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val;
            if (!multidictFind(restored, &key, &val)) {
                ERR("Key %d not found", i);
            } else if (val.data.i != i * 1000) {
                ERR("Key %d: expected value %d, got %" PRIdMAX, i, i * 1000,
                    val.data.i);
            }
        }

        testMultidictFreeWithClass(restored);
        persistClose(p);
    }

    TEST("WAL replay idempotence") {
        /* Replaying the same WAL twice should give same result */
        cleaner(flexFree) flex *f1 = flexNew();
        /* The second replay target comes straight from persistRestore
         * below, so only the snapshot source is built here. */

        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *snapStore = persistStoreMemory(0);
        persistStore *walStore = persistStoreMemory(0);
        persistAttachSnapshot(p, snapStore);
        persistAttachWAL(p, walStore);

        persistSnapshot(p, f1);

        /* Log operations */
        for (int i = 0; i < 10; i++) {
            databox box = DATABOX_SIGNED(i);
            persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box, 1);
        }
        persistSync(p);

        /* Replay once */
        walStore->seek(walStore->ctx, 0, SEEK_SET);
        flex *r1 = persistRecover(p);

        /* Replay again (simulate restart) */
        walStore->seek(walStore->ctx, 0, SEEK_SET);
        snapStore->seek(snapStore->ctx, 0, SEEK_SET);
        flex *r2 = persistRestore(p);
        persistReplayWAL(p, &r2);

        /* Both should be identical */
        if (flexBytes(r1) != flexBytes(r2)) {
            ERRR("Idempotent replay size mismatch");
        }
        if (memcmp(r1, r2, flexBytes(r1)) != 0) {
            ERRR("Idempotent replay content mismatch");
        }

        flexFree(r1);
        flexFree(r2);
        persistClose(p);
    }

    /* ================================================================
     * Comprehensive Flex Tests
     * ================================================================ */

    TEST("flex: empty structure round-trip") {
        cleaner(flexFree) flex *f = flexNew();

        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, f);

        flex *restored = persistRestore(p);
        if (!restored) {
            ERRR("Empty flex restore failed");
        }
        if (flexCount(restored) != 0) {
            ERR("Empty flex should have 0 elements, got %zu",
                flexCount(restored));
        }

        flexFree(restored);
        persistClose(p);
    }

    TEST("flex: mixed types round-trip") {
        cleaner(flexFree) flex *f = flexNew();

        /* Add various types */
        flexPushSigned(&f, 0, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, -1, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, 1, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT8_MIN, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT8_MAX, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT16_MIN, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT16_MAX, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT32_MIN, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT32_MAX, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT64_MIN, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT64_MAX, FLEX_ENDPOINT_TAIL);

        /* Unsigned values */
        flexPushUnsigned(&f, 0, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, UINT8_MAX, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, UINT16_MAX, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, UINT32_MAX, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, UINT64_MAX, FLEX_ENDPOINT_TAIL);

        /* Strings */
        flexPushBytes(&f, "", 0, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&f, "a", 1, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&f, "hello world", 11, FLEX_ENDPOINT_TAIL);

        /* Binary with nulls */
        const uint8_t binary[] = {0x00, 0x01, 0x02, 0x00, 0xFF};
        flexPushBytes(&f, binary, sizeof(binary), FLEX_ENDPOINT_TAIL);

        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, f);

        flex *restored = persistRestore(p);
        if (!restored) {
            ERRR("Mixed types flex restore failed");
        }

        if (flexCount(restored) != flexCount(f)) {
            ERR("Count mismatch: %zu vs %zu", flexCount(restored),
                flexCount(f));
        }

        /* Verify byte-for-byte equality */
        if (flexBytes(restored) != flexBytes(f)) {
            ERR("Size mismatch: %zu vs %zu", flexBytes(restored), flexBytes(f));
        }
        if (memcmp(f, restored, flexBytes(f)) != 0) {
            ERRR("Content mismatch");
        }

        flexFree(restored);
        persistClose(p);
    }

    TEST("flex: large structure round-trip") {
        cleaner(flexFree) flex *f = flexNew();

        /* Add many elements to stress test */
        for (int i = 0; i < 10000; i++) {
            flexPushSigned(&f, i * 17 - 5000, FLEX_ENDPOINT_TAIL);
        }

        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, f);

        flex *restored = persistRestore(p);
        if (!restored) {
            ERRR("Large flex restore failed");
        }

        if (flexCount(restored) != 10000) {
            ERR("Expected 10000 elements, got %zu", flexCount(restored));
        }

        /* Verify byte-for-byte equality */
        if (memcmp(f, restored, flexBytes(f)) != 0) {
            ERRR("Large flex content mismatch");
        }

        flexFree(restored);
        persistClose(p);
    }

    TEST("flex: WAL with mixed operations") {
        cleaner(flexFree) flex *f = flexNew();

        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *snapStore = persistStoreMemory(0);
        persistStore *walStore = persistStoreMemory(0);
        persistAttachSnapshot(p, snapStore);
        persistAttachWAL(p, walStore);

        /* Initial snapshot with some data */
        flexPushSigned(&f, 100, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, 200, FLEX_ENDPOINT_TAIL);
        persistSnapshot(p, f);

        /* Log various operations */
        databox box1 = DATABOX_SIGNED(50);
        persistLogOp(p, PERSIST_OP_PUSH_HEAD, &box1, 1);

        databox box2 = DATABOX_SIGNED(300);
        persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box2, 1);

        databox box3 = DATABOX_WITH_BYTES((uint8_t *)"test", 4);
        persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box3, 1);

        persistSync(p);

        /* Recover and verify */
        flex *recovered = persistRecover(p);
        if (!recovered) {
            ERRR("WAL recovery failed");
        }

        if (flexCount(recovered) != 5) {
            ERR("Expected 5 elements, got %zu", flexCount(recovered));
        }

        /* Verify order: 50, 100, 200, 300, "test" */
        flexEntry *fe = flexHead(recovered);
        databox box;
        int64_t expected[] = {50, 100, 200, 300};
        for (int i = 0; i < 4; i++) {
            flexGetByType(fe, &box);
            if (box.data.i != expected[i]) {
                ERR("Element %d: expected %" PRId64 ", got %" PRIdMAX, i,
                    expected[i], box.data.i);
            }
            fe = flexNext(recovered, fe);
        }

        flexFree(recovered);
        persistClose(p);
    }

    /* ================================================================
     * Comprehensive Intset Tests
     * ================================================================ */

    TEST("intset: empty structure round-trip") {
        cleaner(intsetFree) intset *is = intsetNew();

        persist *p = persistCreate(&persistOpsIntset, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, is);

        intset *restored = persistRestore(p);
        if (!restored) {
            ERRR("Empty intset restore failed");
        }
        if (intsetCount(restored) != 0) {
            ERR("Empty intset should have 0 elements, got %zu",
                intsetCount(restored));
        }

        intsetFree(restored);
        persistClose(p);
    }

    TEST("intset: small tier values only (int16)") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        /* Add values that fit in int16 */
        int16_t smallValues[] = {0,     1,      -1,    100,   -100,
                                 32767, -32768, 12345, -12345};
        for (size_t i = 0; i < COUNT_ARRAY(smallValues); i++) {
            intsetAdd(&is, smallValues[i], &success);
        }

        persist *p = persistCreate(&persistOpsIntset, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, is);

        intset *restored = persistRestore(p);
        if (!restored) {
            ERRR("Small tier intset restore failed");
        }

        if (intsetCount(restored) != COUNT_ARRAY(smallValues)) {
            ERR("Count mismatch: %zu vs %zu", intsetCount(restored),
                COUNT_ARRAY(smallValues));
        }

        for (size_t i = 0; i < COUNT_ARRAY(smallValues); i++) {
            if (!intsetFind(restored, smallValues[i])) {
                ERR("Missing small value: %d", smallValues[i]);
            }
        }

        intsetFree(restored);
        persistClose(p);
    }

    TEST("intset: medium tier values (int16 + int32)") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        /* Mix of int16 and int32 values */
        int64_t values[] = {0,       1,          -1,         32767,
                            -32768,  32768,      -32769,     100000,
                            -100000, 2147483647, -2147483648};
        for (size_t i = 0; i < COUNT_ARRAY(values); i++) {
            intsetAdd(&is, values[i], &success);
        }

        persist *p = persistCreate(&persistOpsIntset, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, is);

        intset *restored = persistRestore(p);
        if (!restored) {
            ERRR("Medium tier intset restore failed");
        }

        if (intsetCount(restored) != COUNT_ARRAY(values)) {
            ERR("Count mismatch: %zu vs %zu", intsetCount(restored),
                COUNT_ARRAY(values));
        }

        for (size_t i = 0; i < COUNT_ARRAY(values); i++) {
            if (!intsetFind(restored, values[i])) {
                ERR("Missing medium value: %" PRId64, values[i]);
            }
        }

        intsetFree(restored);
        persistClose(p);
    }

    TEST("intset: full tier values (int16 + int32 + int64)") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        /* Mix requiring all tiers */
        int64_t values[] = {0,
                            1,
                            -1,
                            INT16_MAX,
                            INT16_MIN,
                            INT32_MAX,
                            INT32_MIN,
                            INT64_MAX,
                            INT64_MIN,
                            (int64_t)INT32_MAX + 1,
                            (int64_t)INT32_MIN - 1,
                            4611686018427387903LL,
                            -4611686018427387903LL};
        for (size_t i = 0; i < COUNT_ARRAY(values); i++) {
            intsetAdd(&is, values[i], &success);
        }

        persist *p = persistCreate(&persistOpsIntset, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, is);

        intset *restored = persistRestore(p);
        if (!restored) {
            ERRR("Full tier intset restore failed");
        }

        if (intsetCount(restored) != COUNT_ARRAY(values)) {
            ERR("Count mismatch: %zu vs %zu", intsetCount(restored),
                COUNT_ARRAY(values));
        }

        for (size_t i = 0; i < COUNT_ARRAY(values); i++) {
            if (!intsetFind(restored, values[i])) {
                ERR("Missing full tier value: %" PRId64, values[i]);
            }
        }

        intsetFree(restored);
        persistClose(p);
    }

    TEST("intset: large count round-trip") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        /* Add many values */
        for (int i = -5000; i < 5000; i++) {
            intsetAdd(&is, i * 3, &success);
        }

        persist *p = persistCreate(&persistOpsIntset, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, is);

        intset *restored = persistRestore(p);
        if (!restored) {
            ERRR("Large intset restore failed");
        }

        if (intsetCount(restored) != 10000) {
            ERR("Expected 10000 elements, got %zu", intsetCount(restored));
        }

        /* Verify all values */
        for (int i = -5000; i < 5000; i++) {
            if (!intsetFind(restored, i * 3)) {
                ERR("Missing value: %d", i * 3);
            }
        }

        intsetFree(restored);
        persistClose(p);
    }

    TEST("intset: WAL insert and delete") {
        cleaner(intsetFree) intset *is = intsetNew();

        persist *p = persistCreate(&persistOpsIntset, NULL);
        persistStore *snapStore = persistStoreMemory(0);
        persistStore *walStore = persistStoreMemory(0);
        persistAttachSnapshot(p, snapStore);
        persistAttachWAL(p, walStore);

        /* Start with some values */
        bool success;
        intsetAdd(&is, 10, &success);
        intsetAdd(&is, 20, &success);
        intsetAdd(&is, 30, &success);
        persistSnapshot(p, is);

        /* Log inserts */
        int64_t v1 = 40;
        persistLogOp(p, PERSIST_OP_INSERT, &v1, 1);
        int64_t v2 = 50;
        persistLogOp(p, PERSIST_OP_INSERT, &v2, 1);

        /* Log delete */
        int64_t v3 = 20;
        persistLogOp(p, PERSIST_OP_DELETE, &v3, 1);

        persistSync(p);

        intset *recovered = persistRecover(p);
        if (!recovered) {
            ERRR("Intset WAL recovery failed");
        }

        /* Should have: 10, 30, 40, 50 (not 20) */
        if (intsetCount(recovered) != 4) {
            ERR("Expected 4 elements, got %zu", intsetCount(recovered));
        }

        if (!intsetFind(recovered, 10) || !intsetFind(recovered, 30) ||
            !intsetFind(recovered, 40) || !intsetFind(recovered, 50)) {
            ERRR("Missing expected values");
        }
        if (intsetFind(recovered, 20)) {
            ERRR("Deleted value 20 still present");
        }

        intsetFree(recovered);
        persistClose(p);
    }

    /* ================================================================
     * Comprehensive Multimap Tests
     * ================================================================ */

    TEST("multimap: empty structure round-trip") {
        cleaner(multimapFree) multimap *m = multimapNew(2);

        persist *p = persistCreate(&persistOpsMultimap, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, m);

        multimap *restored = persistRestore(p);
        if (!restored) {
            ERRR("Empty multimap restore failed");
        }
        if (multimapCount(restored) != 0) {
            ERR("Empty multimap should have 0 elements, got %zu",
                multimapCount(restored));
        }

        multimapFree(restored);
        persistClose(p);
    }

    TEST("multimap: various element widths") {
        /* Test with 2, 3, 4, 5 element entries */
        for (uint32_t width = 2; width <= 5; width++) {
            cleaner(multimapFree) multimap *m = multimapNew(width);

            /* Add entries */
            for (int i = 0; i < 100; i++) {
                databox elements[width];
                const databox *ptrs[width];
                for (uint32_t j = 0; j < width; j++) {
                    elements[j] = DATABOX_SIGNED(i * width + j);
                    ptrs[j] = &elements[j];
                }
                multimapInsertFullWidth(&m, ptrs);
            }

            persist *p = persistCreate(&persistOpsMultimap, NULL);
            persistStore *store = persistStoreMemory(0);
            persistAttachSnapshot(p, store);
            persistSnapshot(p, m);

            multimap *restored = persistRestore(p);
            if (!restored) {
                ERR("Multimap width %u restore failed", width);
            } else if (multimapCount(restored) != 100) {
                ERR("Width %u: expected 100 entries, got %zu", width,
                    multimapCount(restored));
            }

            multimapFree(restored);
            persistClose(p);
        }
    }

    TEST("multimap: mixed key types") {
        cleaner(multimapFree) multimap *m = multimapNew(2);

        /* Integer keys */
        for (int i = 0; i < 50; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 100);
            const databox *entry[] = {&key, &val};
            multimapInsert(&m, entry);
        }

        /* String keys */
        const char *stringKeys[] = {"alpha", "beta", "gamma", "delta",
                                    "epsilon"};
        for (size_t i = 0; i < COUNT_ARRAY(stringKeys); i++) {
            databox key = DATABOX_WITH_BYTES((uint8_t *)stringKeys[i],
                                             strlen(stringKeys[i]));
            databox val = DATABOX_SIGNED((int64_t)i * 1000);
            const databox *entry[] = {&key, &val};
            multimapInsert(&m, entry);
        }

        persist *p = persistCreate(&persistOpsMultimap, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, m);

        multimap *restored = persistRestore(p);
        if (!restored) {
            ERRR("Mixed key multimap restore failed");
        }

        if (multimapCount(restored) != 55) {
            ERR("Expected 55 entries, got %zu", multimapCount(restored));
        }

        /* Verify integer keys */
        for (int i = 0; i < 50; i++) {
            databox key = DATABOX_SIGNED(i);
            if (!multimapExists(restored, &key)) {
                ERR("Missing integer key: %d", i);
            }
        }

        /* Verify string keys */
        for (size_t i = 0; i < COUNT_ARRAY(stringKeys); i++) {
            databox key = DATABOX_WITH_BYTES((uint8_t *)stringKeys[i],
                                             strlen(stringKeys[i]));
            if (!multimapExists(restored, &key)) {
                ERR("Missing string key: %s", stringKeys[i]);
            }
        }

        multimapFree(restored);
        persistClose(p);
    }

    TEST("multimap: large structure triggers tier transitions") {
        cleaner(multimapFree) multimap *m = multimapNew(2);

        /* Add enough entries to trigger Small -> Medium -> Full transitions */
        for (int i = 0; i < 5000; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 7 - 1000);
            const databox *entry[] = {&key, &val};
            multimapInsert(&m, entry);
        }

        persist *p = persistCreate(&persistOpsMultimap, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, m);

        multimap *restored = persistRestore(p);
        if (!restored) {
            ERRR("Large multimap restore failed");
        }

        if (multimapCount(restored) != 5000) {
            ERR("Expected 5000 entries, got %zu", multimapCount(restored));
        }

        /* Spot check some values */
        for (int i = 0; i < 5000; i += 500) {
            databox key = DATABOX_SIGNED(i);
            databox val;
            databox *vals[] = {&val};
            if (!multimapLookup(restored, &key, vals)) {
                ERR("Missing key %d", i);
            } else if (val.data.i != i * 7 - 1000) {
                ERR("Key %d: expected %d, got %" PRIdMAX, i, i * 7 - 1000,
                    val.data.i);
            }
        }

        multimapFree(restored);
        persistClose(p);
    }

    /* ================================================================
     * Comprehensive Multilist Tests
     * ================================================================ */

    TEST("multilist: empty structure round-trip") {
        cleaner(multilistFree) multilist *ml =
            multilistNew(FLEX_CAP_LEVEL_2048, 0);

        persist *p = persistCreate(&persistOpsMultilist, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, ml);

        multilist *restored = persistRestore(p);
        if (!restored) {
            ERRR("Empty multilist restore failed");
        }
        if (multilistCount(restored) != 0) {
            ERR("Empty multilist should have 0 elements, got %zu",
                multilistCount(restored));
        }

        multilistFree(restored);
        persistClose(p);
    }

    TEST("multilist: various element types") {
        cleaner(multilistFree) multilist *ml =
            multilistNew(FLEX_CAP_LEVEL_2048, 0);
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();

        /* Integers */
        for (int i = -100; i <= 100; i++) {
            databox box = DATABOX_SIGNED(i);
            multilistPushByTypeTail(&ml, state, &box);
        }

        /* Large integers */
        int64_t largeVals[] = {INT64_MIN, INT64_MAX, INT32_MIN, INT32_MAX};
        for (size_t i = 0; i < COUNT_ARRAY(largeVals); i++) {
            databox box = DATABOX_SIGNED(largeVals[i]);
            multilistPushByTypeTail(&ml, state, &box);
        }

        /* Strings */
        const char *strings[] = {"hello", "world", "",
                                 "test string with spaces"};
        for (size_t i = 0; i < COUNT_ARRAY(strings); i++) {
            databox box =
                DATABOX_WITH_BYTES((uint8_t *)strings[i], strlen(strings[i]));
            multilistPushByTypeTail(&ml, state, &box);
        }

        persist *p = persistCreate(&persistOpsMultilist, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, ml);

        multilist *restored = persistRestore(p);
        if (!restored) {
            ERRR("Mixed multilist restore failed");
        }

        size_t expectedCount =
            201 + COUNT_ARRAY(largeVals) + COUNT_ARRAY(strings);
        if (multilistCount(restored) != expectedCount) {
            ERR("Expected %zu elements, got %zu", expectedCount,
                multilistCount(restored));
        }

        multilistFree(restored);
        persistClose(p);
    }

    TEST("multilist: large multi-node structure") {
        cleaner(multilistFree) multilist *ml =
            multilistNew(FLEX_CAP_LEVEL_512, 0);
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();

        /* Add enough to create multiple nodes */
        for (int i = 0; i < 10000; i++) {
            databox box = DATABOX_SIGNED(i);
            multilistPushByTypeTail(&ml, state, &box);
        }

        persist *p = persistCreate(&persistOpsMultilist, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, ml);

        multilist *restored = persistRestore(p);
        if (!restored) {
            ERRR("Large multilist restore failed");
        }

        if (multilistCount(restored) != 10000) {
            ERR("Expected 10000 elements, got %zu", multilistCount(restored));
        }

        /* Verify some values */
        cleaner(mflexStateFree) mflexState *s2 = mflexStateCreate();
        for (int i = 0; i < 10000; i += 1000) {
            multilistEntry entry;
            if (!multilistIndex(restored, s2, i, &entry, true)) {
                ERR("Failed to get index %d", i);
            } else if (entry.box.data.i != i) {
                ERR("Index %d: expected %d, got %" PRIdMAX, i, i,
                    entry.box.data.i);
            }
        }

        multilistFree(restored);
        persistClose(p);
    }

    /* ================================================================
     * Comprehensive Multidict Tests
     * ================================================================ */

    TEST("multidict: empty structure round-trip") {
        /* @own: container; free=multidictDefaultClassFree */
        multidictClass *qdc = multidictDefaultClassNew();
        cleanerName(testMultidictFreeWithClass, Dict) multidict *d =
            multidictNew(&multidictTypeExactKey, qdc, 0);

        persist *p = persistCreate(&persistOpsMultidict, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, d);

        multidict *restored = persistRestore(p);
        if (!restored) {
            ERRR("Empty multidict restore failed");
        }
        if (multidictCount(restored) != 0) {
            ERR("Empty multidict should have 0 elements, got %" PRIu64,
                multidictCount(restored));
        }

        testMultidictFreeWithClass(restored);
        persistClose(p);
    }

    TEST("multidict: large hash table") {
        /* @own: container; free=multidictDefaultClassFree */
        multidictClass *qdc = multidictDefaultClassNew();
        cleanerName(testMultidictFreeWithClass, Dict) multidict *d =
            multidictNew(&multidictTypeExactKey, qdc, 0);

        /* Add many entries to test bucket distribution */
        for (int i = 0; i < 1000; i++) {
            databox key = DATABOX_SIGNED(i * 17); /* Spread keys */
            databox val = DATABOX_SIGNED(i);
            multidictAdd(d, &key, &val);
        }

        persist *p = persistCreate(&persistOpsMultidict, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, d);

        multidict *restored = persistRestore(p);
        if (!restored) {
            ERRR("Large multidict restore failed");
        }

        if (multidictCount(restored) != 1000) {
            ERR("Expected 1000 entries, got %" PRIu64,
                multidictCount(restored));
        }

        /* Verify all entries */
        for (int i = 0; i < 1000; i++) {
            databox key = DATABOX_SIGNED(i * 17);
            databox val;
            if (!multidictFind(restored, &key, &val)) {
                ERR("Missing key %d", i * 17);
            } else if (val.data.i != i) {
                ERR("Key %d: expected value %d, got %" PRIdMAX, i * 17, i,
                    val.data.i);
            }
        }

        testMultidictFreeWithClass(restored);
        persistClose(p);
    }

    TEST("multidict: case keys through snapshot and WAL replay") {
        /* @own: container; free=multidictDefaultClassFree */
        multidictClass *qdc = multidictDefaultClassNew();
        cleanerName(testMultidictFreeWithClass, Dict) multidict *d =
            multidictNew(&multidictTypeCaseKey, qdc, 0);
        const databox alpha = databoxNewBytesString("Alpha");
        const databox one = DATABOX_SIGNED(1);
        multidictAdd(d, &alpha, &one);

        persist *p = persistCreate(&persistOpsMultidict, NULL);
        persistStore *snapStore = persistStoreMemory(0);
        persistStore *walStore = persistStoreMemory(0);
        persistAttachSnapshot(p, snapStore);
        persistAttachWAL(p, walStore);
        persistSnapshot(p, d);

        /* Byte-string keys and values of any length and a 128-bit key
         * replay; every spelling of "alpha" is the snapshot's entry */
        const databox ALPHA = databoxNewBytesString("ALPHA");
        const databox text = databoxNewBytesString("a byte-string value");
        const databox beta = databoxNewBytesString("Beta");
        const databox betaLow = databoxNewBytesString("beta");
        const databox alphaLow = databoxNewBytesString("alpha");
        const databox two = DATABOX_SIGNED(2);
        const databox three = DATABOX_SIGNED(3);
        databoxBig wide;
        DATABOX_BIG_UNSIGNED_128(&wide, ((__uint128_t)1 << 100) + 5);
        const databox *ins1[] = {&ALPHA, &text};
        const databox *ins2[] = {&beta, &two};
        const databox *rep[] = {&alphaLow, &three};
        const databox *ins3[] = {(const databox *)&wide, &two};
        persistLogOp(p, PERSIST_OP_INSERT, ins1, 2);
        persistLogOp(p, PERSIST_OP_INSERT, ins2, 2);
        persistLogOp(p, PERSIST_OP_DELETE, &betaLow, 1);
        persistLogOp(p, PERSIST_OP_REPLACE, rep, 2);
        persistLogOp(p, PERSIST_OP_INSERT, ins3, 2);
        persistSync(p);

        walStore->seek(walStore->ctx, 0, SEEK_SET);
        snapStore->seek(snapStore->ctx, 0, SEEK_SET);
        multidict *restored = persistRecover(p);
        databox v = {{0}};
        databoxBig wv;
        DATABOX_BIG_INIT(&wv);
        if (!restored || multidictGetType(restored) != &multidictTypeCaseKey ||
            multidictCount(restored) != 2 ||
            !multidictFind(restored, &ALPHA, &v) || !DATABOX_IS_INTEGER(&v) ||
            v.data.i != 3 || multidictExists(restored, &beta) ||
            !multidictFind(restored, (const databox *)&wide, (databox *)&wv) ||
            !DATABOX_IS_INTEGER(&wv) || wv.data.i != 2) {
            ERRR("case multidict did not replay as a case dict");
        }

        if (restored) {
            bool alphaKept = false;
            multidictIterator iter;
            multidictIteratorInit(restored, &iter);
            multidictEntry e;
            while (multidictIteratorNext(&iter, &e)) {
                alphaKept = alphaKept ||
                            (DATABOX_IS_BYTES(&e.key) && e.key.len == 5 &&
                             memcmp(databoxCBytes(&e.key), "Alpha", 5) == 0);
            }
            multidictIteratorRelease(&iter);
            if (!alphaKept) {
                ERRR("case multidict replay lost the stored spelling");
            }
            testMultidictFreeWithClass(restored);
        }
        persistClose(p);
    }

    TEST("exact persistence: numeral strings stay strings") {
        /* Persistence stores values exactly as the container held them:
         * "22" stays the 2-byte string and the integer 22 stays a
         * separate integer, through the snapshot and through WAL replay. */
        const databox s22 = databoxNewBytesString("22");
        const databox s007 = databoxNewBytesString("007");
        const databox s1e3 = databoxNewBytesString("1e3");
        const databox s42 = databoxNewBytesString("42");
        const databox i22 = DATABOX_SIGNED(22);

        /* multidict: one key live and in the snapshot, one via the WAL. */
        {
            /* @own: container; free=multidictDefaultClassFree */
            multidictClass *qdc = multidictDefaultClassNew();
            cleanerName(testMultidictFreeWithClass, Dict) multidict *d =
                multidictNew(&multidictTypeExactKey, qdc, 0);
            multidictAdd(d, &s22, &s42);
            multidictAdd(d, &i22, &s007);

            persist *p = persistCreate(&persistOpsMultidict, NULL);
            persistStore *snapStore = persistStoreMemory(0);
            persistStore *walStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);
            persistAttachWAL(p, walStore);
            persistSnapshot(p, d);
            const databox *ins[] = {&s1e3, &s22};
            persistLogOp(p, PERSIST_OP_INSERT, ins, 2);
            persistSync(p);

            walStore->seek(walStore->ctx, 0, SEEK_SET);
            snapStore->seek(snapStore->ctx, 0, SEEK_SET);
            multidict *r = persistRecover(p);
            databox v = {{0}};
            if (!r || multidictCount(r) != 3) {
                ERRR("multidict: recovered count is not 3");
            } else {
                if (!multidictFind(r, &s22, &v) || !DATABOX_IS_BYTES(&v) ||
                    databoxCompare(&v, &s42) != 0) {
                    ERRR("multidict: \"22\" -> \"42\" did not survive");
                }
                if (!multidictFind(r, &i22, &v) || !DATABOX_IS_BYTES(&v) ||
                    databoxCompare(&v, &s007) != 0) {
                    ERRR("multidict: 22 -> \"007\" did not survive");
                }
                if (!multidictFind(r, &s1e3, &v) || !DATABOX_IS_BYTES(&v) ||
                    databoxCompare(&v, &s22) != 0) {
                    ERRR("multidict: WAL \"1e3\" -> \"22\" did not replay");
                }
            }
            if (r) {
                testMultidictFreeWithClass(r);
            }
            persistClose(p);
        }

        /* multimap: the snapshot is a byte copy; the WAL row must match. */
        {
            cleaner(multimapFree) multimap *m = multimapNew(2);
            persist *p = persistCreate(&persistOpsMultimap, NULL);
            persistStore *snapStore = persistStoreMemory(0);
            persistStore *walStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);
            persistAttachWAL(p, walStore);
            persistSnapshot(p, m);
            const databox *row[] = {&s22, &s42};
            multimapInsert(&m, row);
            persistLogOp(p, PERSIST_OP_INSERT, row, 2);
            persistSync(p);

            walStore->seek(walStore->ctx, 0, SEEK_SET);
            snapStore->seek(snapStore->ctx, 0, SEEK_SET);
            multimap *r = persistRecover(p);
            if (!r || multimapCount(r) != 1 || !multimapExists(r, &s22) ||
                multimapExists(r, &i22)) {
                ERRR("multimap: WAL row \"22\" did not replay as a string");
            }
            if (r) {
                multimapFree(r);
            }
            persistClose(p);
        }

        /* multilist: pushes and the WAL keep strings and integers apart. */
        {
            cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
            persistSetReplayState(state);
            cleaner(multilistFree) multilist *ml =
                multilistNew(FLEX_CAP_LEVEL_2048, 0);
            multilistPushByTypeTail(&ml, state, &s22);
            multilistPushByTypeTail(&ml, state, &i22);

            persist *p = persistCreate(&persistOpsMultilist, NULL);
            persistStore *snapStore = persistStoreMemory(0);
            persistStore *walStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);
            persistAttachWAL(p, walStore);
            persistSnapshot(p, ml);
            multilistPushByTypeTail(&ml, state, &s007);
            persistLogOp(p, PERSIST_OP_PUSH_TAIL, &s007, 1);
            persistSync(p);

            walStore->seek(walStore->ctx, 0, SEEK_SET);
            snapStore->seek(snapStore->ctx, 0, SEEK_SET);
            multilist *r = persistRecover(p);
            const databox *want[] = {&s22, &i22, &s007};
            if (!r || multilistCount(r) != COUNT_ARRAY(want)) {
                ERRR("multilist: recovered count is not 3");
            } else {
                for (size_t i = 0; i < COUNT_ARRAY(want); i++) {
                    multilistEntry e = {0};
                    /* A string stays a string and a number a number; a
                     * number may come back in its minimal same-type
                     * form (a non-negative SIGNED reads as UNSIGNED). */
                    if (!multilistIndexGet(r, state, (mlOffsetId)i, &e) ||
                        DATABOX_IS_BYTES(&e.box) != DATABOX_IS_BYTES(want[i]) ||
                        databoxCompare(&e.box, want[i]) != 0) {
                        ERR("multilist: element %zu changed type or value", i);
                    }
                }
            }
            if (r) {
                multilistFree(r);
            }
            persistClose(p);
            persistSetReplayState(NULL);
        }

        /* multiOrderedSet: a "22" member is not the integer 22. */
        {
            cleaner(multiOrderedSetFree) multiOrderedSet *mos =
                multiOrderedSetNew();
            const databox score = DATABOX_DOUBLE(1.5);
            multiOrderedSetAdd(&mos, &score, &s22);

            persist *p = persistCreate(&persistOpsMultiOrderedSet, NULL);
            persistStore *snapStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);
            persistSnapshot(p, mos);
            snapStore->seek(snapStore->ctx, 0, SEEK_SET);
            multiOrderedSet *r = persistRestore(p);
            if (!r || multiOrderedSetCount(r) != 1 ||
                !multiOrderedSetExists(r, &s22) ||
                multiOrderedSetExists(r, &i22)) {
                ERRR("multiOrderedSet: member \"22\" did not survive");
            }
            if (r) {
                multiOrderedSetFree(r);
            }
            persistClose(p);
        }
    }

    TEST("multidict: unmarked WAL records still apply") {
        /* @own: container; free=multidictDefaultClassFree */
        multidictClass *qdc = multidictDefaultClassNew();
        cleanerName(testMultidictFreeWithClass, Dict) multidict *d =
            multidictNew(&multidictTypeExactKey, qdc, 0);

        /* The layout written before records were marked:
         * [keyType][key][valType][val] and [keyType][key] */
        const databox k7 = DATABOX_SIGNED(7);
        const databox k8 = DATABOX_SIGNED(8);
        const databox hello = databoxNewBytesString("hello");
        const databox nine = DATABOX_SIGNED(9);
        const databox *pairs[2][2] = {{&k7, &hello}, {&k8, &nine}};
        for (size_t i = 0; i < 2; i++) {
            uint8_t rec[64];
            size_t n = 0;
            for (size_t j = 0; j < 2; j++) {
                databoxLinear dl;
                size_t partLen;
                const void *part;
                uint8_t partType;
                DATABOX_LINEAR_PARTS_ENCODE(pairs[i][j], partLen, part,
                                            partType, &dl);
                rec[n++] = partType;
                memcpy(rec + n, part, partLen);
                n += partLen;
            }
            if (!persistOpsMultidict.applyOp(&d, PERSIST_OP_INSERT, rec, n)) {
                ERR("unmarked insert record %zu was refused", i);
            }
        }

        databoxLinear dl;
        size_t partLen;
        const void *part;
        uint8_t partType;
        DATABOX_LINEAR_PARTS_ENCODE(&k8, partLen, part, partType, &dl);
        uint8_t del[16];
        del[0] = partType;
        memcpy(del + 1, part, partLen);
        if (!persistOpsMultidict.applyOp(&d, PERSIST_OP_DELETE, del,
                                         1 + partLen)) {
            ERRR("unmarked delete record was refused");
        }

        /* A byte-string value is the rest of the record */
        databox v = {{0}};
        if (multidictCount(d) != 1 || !multidictFind(d, &k7, &v) ||
            !DATABOX_IS_BYTES(&v) || v.len != 5 ||
            memcmp(databoxCBytes(&v), "hello", 5) != 0) {
            ERRR("unmarked records did not apply");
        }

        /* An unmarked record with a byte-string key has no key length
         * and is refused rather than misread */
        const uint8_t bytesKey[] = {DATABOX_LINEAR_TYPE_BYTES, 'k', 2, 1};
        if (persistOpsMultidict.applyOp(&d, PERSIST_OP_INSERT, bytesKey,
                                        sizeof(bytesKey)) ||
            multidictCount(d) != 1) {
            ERRR("unmarked byte-string key record was not refused");
        }
    }

    /* ================================================================
     * Fuzz Tests
     * ================================================================ */

    TEST("FUZZ: flex random operations") {
        uint32_t seed = 12345;
        cleaner(flexFree) flex *f = flexNew();

        /* Perform random operations */
        for (int iter = 0; iter < 500; iter++) {
            seed = seed * 1103515245 + 12345;
            int op = seed % 4;

            switch (op) {
            case 0: { /* Push signed */
                int64_t val = (int64_t)(seed % 1000000) - 500000;
                flexPushSigned(&f, val, FLEX_ENDPOINT_TAIL);
                break;
            }
            case 1: { /* Push unsigned */
                uint64_t val = seed % 1000000;
                flexPushUnsigned(&f, val, FLEX_ENDPOINT_TAIL);
                break;
            }
            case 2: { /* Push bytes */
                char buf[32];
                int len = snprintf(buf, sizeof(buf), "str%u", seed % 10000);
                flexPushBytes(&f, buf, len, FLEX_ENDPOINT_TAIL);
                break;
            }
            case 3: { /* Push head */
                flexPushSigned(&f, (int64_t)seed, FLEX_ENDPOINT_HEAD);
                break;
            }
            }
        }

        /* Snapshot and restore */
        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, f);

        flex *restored = persistRestore(p);
        if (!restored) {
            ERRR("Fuzz flex restore failed");
        }

        if (flexCount(restored) != flexCount(f)) {
            ERR("Fuzz flex count: %zu vs %zu", flexCount(restored),
                flexCount(f));
        }
        if (flexBytes(restored) != flexBytes(f)) {
            ERR("Fuzz flex size: %zu vs %zu", flexBytes(restored),
                flexBytes(f));
        }
        if (memcmp(f, restored, flexBytes(f)) != 0) {
            ERRR("Fuzz flex content mismatch");
        }

        flexFree(restored);
        persistClose(p);
    }

    TEST("FUZZ: intset random values") {
        uint64_t seed = 67890;
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        /* Add random values across different ranges */
        int64_t addedValues[1000];
        size_t addedCount = 0;

        for (int iter = 0; iter < 1000; iter++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;

            int64_t val;
            int range = seed % 4;
            switch (range) {
            case 0: /* int16 range */
                val = (int64_t)(seed % 65536) - 32768;
                break;
            case 1: /* int32 range */
                val = (int64_t)(seed % 4294967296ULL) - 2147483648LL;
                break;
            case 2: /* Extreme values */
                val = (seed % 2) ? INT64_MAX - (seed % 1000)
                                 : INT64_MIN + (seed % 1000);
                break;
            default: /* Full int64 range */
                val = (int64_t)seed;
                break;
            }

            intsetAdd(&is, val, &success);
            if (success && addedCount < COUNT_ARRAY(addedValues)) {
                addedValues[addedCount++] = val;
            }
        }

        /* Snapshot and restore */
        persist *p = persistCreate(&persistOpsIntset, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, is);

        intset *restored = persistRestore(p);
        if (!restored) {
            ERRR("Fuzz intset restore failed");
        }

        if (intsetCount(restored) != intsetCount(is)) {
            ERR("Fuzz intset count: %zu vs %zu", intsetCount(restored),
                intsetCount(is));
        }

        /* Verify all values we tracked */
        for (size_t i = 0; i < addedCount; i++) {
            if (!intsetFind(restored, addedValues[i])) {
                ERR("Fuzz intset missing value: %" PRId64, addedValues[i]);
            }
        }

        intsetFree(restored);
        persistClose(p);
    }

    TEST("FUZZ: multimap random entries") {
        uint32_t seed = 11111;
        /* key, val1, val2 */
        cleaner(multimapFree) multimap *m = multimapNew(3);

        for (int iter = 0; iter < 500; iter++) {
            seed = seed * 1103515245 + 12345;

            databox key, val1, val2;
            char buf[16]; /* Must be outside if block for scope */

            /* Random key type */
            if (seed % 3 == 0) {
                int len = snprintf(buf, sizeof(buf), "k%u", seed % 10000);
                key = DATABOX_WITH_BYTES((uint8_t *)buf, len);
            } else {
                key = DATABOX_SIGNED((int64_t)(seed % 100000) - 50000);
            }

            val1 = DATABOX_SIGNED((int64_t)seed);
            val2 = DATABOX_UNSIGNED(seed % UINT32_MAX);

            const databox *entry[] = {&key, &val1, &val2};
            multimapInsertFullWidth(&m, entry);
        }

        persist *p = persistCreate(&persistOpsMultimap, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, m);

        multimap *restored = persistRestore(p);
        if (!restored) {
            ERRR("Fuzz multimap restore failed");
        }

        /* Note: counts should match - the multimap stores all entries */
        size_t origCount = multimapCount(m);
        size_t restoredCount = multimapCount(restored);
        if (restoredCount != origCount) {
            ERR("Fuzz multimap count: %zu vs %zu", restoredCount, origCount);
        }

        multimapFree(restored);
        persistClose(p);
    }

    TEST("FUZZ: multilist random push/pop") {
        uint32_t seed = 22222;
        cleaner(multilistFree) multilist *ml =
            multilistNew(FLEX_CAP_LEVEL_1024, 0);
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();

        for (int iter = 0; iter < 1000; iter++) {
            seed = seed * 1103515245 + 12345;
            int op = seed % 5;
            char buf[32]; /* Keep in scope for the whole iteration */
            databox box;

            switch (op) {
            case 0:
            case 1: { /* Push tail (more common) */
                box = DATABOX_SIGNED((int64_t)seed - INT32_MAX);
                multilistPushByTypeTail(&ml, state, &box);
                break;
            }
            case 2: { /* Push head */
                box = DATABOX_SIGNED((int64_t)seed);
                multilistPushByTypeHead(&ml, state, &box);
                break;
            }
            case 3: { /* Push string */
                int len = snprintf(buf, sizeof(buf), "item%u", seed % 10000);
                box = DATABOX_WITH_BYTES((uint8_t *)buf, len);
                multilistPushByTypeTail(&ml, state, &box);
                break;
            }
            case 4: { /* Pop (if not empty) */
                if (multilistCount(ml) > 0) {
                    databox got = {{0}};
                    multilistPop(&ml, state, &got, FLEX_ENDPOINT_TAIL);
                    databoxFreeData(&got);
                }
                break;
            }
            }
        }

        persist *p = persistCreate(&persistOpsMultilist, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, ml);

        multilist *restored = persistRestore(p);
        if (!restored) {
            ERRR("Fuzz multilist restore failed");
        }

        if (multilistCount(restored) != multilistCount(ml)) {
            ERR("Fuzz multilist count: %zu vs %zu", multilistCount(restored),
                multilistCount(ml));
        }

        multilistFree(restored);
        persistClose(p);
    }

    TEST("FUZZ: WAL replay stress test") {
        cleaner(flexFree) flex *f = flexNew();

        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *snapStore = persistStoreMemory(0);
        persistStore *walStore = persistStoreMemory(0);
        persistAttachSnapshot(p, snapStore);
        persistAttachWAL(p, walStore);

        /* Initial snapshot */
        persistSnapshot(p, f);

        /* Log many operations */
        uint32_t seed = 33333;
        for (int iter = 0; iter < 200; iter++) {
            seed = seed * 1103515245 + 12345;
            int op = seed % 3;

            switch (op) {
            case 0: {
                databox box = DATABOX_SIGNED((int64_t)seed);
                persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box, 1);
                break;
            }
            case 1: {
                databox box = DATABOX_SIGNED(-(int64_t)seed);
                persistLogOp(p, PERSIST_OP_PUSH_HEAD, &box, 1);
                break;
            }
            case 2: {
                char buf[16];
                int len = snprintf(buf, sizeof(buf), "w%u", seed % 1000);
                databox box = DATABOX_WITH_BYTES((uint8_t *)buf, len);
                persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box, 1);
                break;
            }
            }
        }
        persistSync(p);

        /* Recover */
        flex *recovered = persistRecover(p);
        if (!recovered) {
            ERRR("WAL stress recovery failed");
        }

        if (flexCount(recovered) != 200) {
            ERR("WAL stress: expected 200 elements, got %zu",
                flexCount(recovered));
        }

        flexFree(recovered);
        persistClose(p);
    }

    TEST("FUZZ: multiple snapshot-restore cycles") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        /* Add initial values */
        for (int i = 0; i < 100; i++) {
            intsetAdd(&is, i * 10, &success);
        }

        /* Perform multiple save/restore cycles */
        for (int cycle = 0; cycle < 10; cycle++) {
            persist *p = persistCreate(&persistOpsIntset, NULL);
            persistStore *store = persistStoreMemory(0);
            persistAttachSnapshot(p, store);
            persistSnapshot(p, is);

            intset *restored = persistRestore(p);
            if (!restored) {
                ERR("Cycle %d: restore failed", cycle);
                persistClose(p);
                break;
            }

            if (intsetCount(restored) != intsetCount(is)) {
                ERR("Cycle %d: count mismatch %zu vs %zu", cycle,
                    intsetCount(restored), intsetCount(is));
            }

            /* Add more values for next cycle */
            for (int i = 0; i < 10; i++) {
                intsetAdd(&restored, (cycle + 1) * 1000 + i, &success);
            }

            intsetFree(is);
            is = restored;
            persistClose(p);
        }

        /* Final verification */
        if (intsetCount(is) != 200) { /* 100 initial + 10*10 added */
            ERR("Final count: expected 200, got %zu", intsetCount(is));
        }
    }

    TEST("FUZZ: compaction preserves data") {
        cleaner(flexFree) flex *f = flexNew();

        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *snapStore = persistStoreMemory(0);
        persistStore *walStore = persistStoreMemory(0);
        persistAttachSnapshot(p, snapStore);
        persistAttachWAL(p, walStore);

        /* Initial data */
        for (int i = 0; i < 50; i++) {
            flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
        }
        persistSnapshot(p, f);

        /* Add WAL entries */
        for (int i = 50; i < 100; i++) {
            databox box = DATABOX_SIGNED(i);
            persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box, 1);
        }
        persistSync(p);

        /* Apply WAL to f for compaction */
        flex *withWal = persistRecover(p);
        if (!withWal) {
            ERRR("Pre-compaction recovery failed");
        }

        /* Compact */
        if (!persistCompact(p, withWal)) {
            ERRR("Compaction failed");
        }

        /* Restore from compacted snapshot */
        flex *afterCompact = persistRestore(p);
        if (!afterCompact) {
            ERRR("Post-compaction restore failed");
        }

        if (flexCount(afterCompact) != 100) {
            ERR("Post-compaction: expected 100, got %zu",
                flexCount(afterCompact));
        }

        /* Verify all values */
        flexEntry *fe = flexHead(afterCompact);
        for (int i = 0; i < 100; i++) {
            databox box;
            flexGetByType(fe, &box);
            if (box.data.i != i) {
                ERR("Post-compaction element %d: expected %d, got %" PRIdMAX, i,
                    i, box.data.i);
            }
            fe = flexNext(afterCompact, fe);
        }

        flexFree(withWal);
        flexFree(afterCompact);
        persistClose(p);
    }

    TEST("FUZZ: concurrent-like WAL and snapshot operations") {
        /* Coherent apply+log semantics: every op both mutates the
         * structure and is logged, so a snapshot always reflects
         * exactly the ops logged before it. This is the invariant the
         * snapshot walSequence watermark relies on - ops already folded
         * into a snapshot (all below its watermark) must NOT be
         * replayed again, and ops logged after it must be. */
        cleaner(multilistFree) multilist *ml =
            multilistNew(FLEX_CAP_LEVEL_512, 0);
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();

        persist *p = persistCreate(&persistOpsMultilist, NULL);
        persistStore *snapStore = persistStoreMemory(0);
        persistStore *walStore = persistStoreMemory(0);
        persistAttachSnapshot(p, snapStore);
        persistAttachWAL(p, walStore);

        int64_t nextVal = 0;
        for (int phase = 0; phase < 5; phase++) {
            for (int i = 0; i < 20; i++) {
                databox box = DATABOX_SIGNED(nextVal++);
                multilistPushByTypeTail(&ml, state, &box);
                persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box, 1);
            }
            /* Snapshot the up-to-date structure: it includes every op
             * logged so far, and the WAL (never truncated here) still
             * holds them all. */
            persistSnapshot(p, ml);
            persistSync(p);
        }

        /* 100 ops folded into the final snapshot, all still present in
         * the WAL below the snapshot watermark - replay must skip every
         * one (no double-apply) and recover exactly 100. */
        multilist *recovered = persistRecover(p);
        if (!recovered) {
            ERRR("Interleaved ops recovery failed");
        }
        if (multilistCount(recovered) != 100) {
            ERR("Snapshot-included ops double-applied: expected 100, got %zu",
                multilistCount(recovered));
        }
        multilistFree(recovered);

        /* Log 10 more ops AFTER the last snapshot; these sit above the
         * watermark and must be replayed on top of it. */
        for (int i = 0; i < 10; i++) {
            databox box = DATABOX_SIGNED(nextVal++);
            multilistPushByTypeTail(&ml, state, &box);
            persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box, 1);
        }
        persistSync(p);

        multilist *recovered2 = persistRecover(p);
        if (!recovered2) {
            ERRR("Post-snapshot WAL recovery failed");
        }
        if (multilistCount(recovered2) != 110) {
            ERR("Post-snapshot ops not replayed: expected 110, got %zu",
                multilistCount(recovered2));
        }

        multilistFree(recovered2);
        persistClose(p);
    }

    TEST("durability: atomic snapshot publish leaves no torn temp file") {
        cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
        char snapPath[PATH_MAX];
        char walPath[PATH_MAX];
        char tmpPath[PATH_MAX];
        if (!dkTestTmpDirPath(&tmp, "atomic.snap", snapPath,
                              sizeof(snapPath)) ||
            !dkTestTmpDirPath(&tmp, "atomic.wal", walPath, sizeof(walPath)) ||
            !dkTestTmpDirPath(&tmp, "atomic.snap.tmp", tmpPath,
                              sizeof(tmpPath))) {
            ERR("could not create a scratch directory: %s", strerror(errno));
            return err;
        }

        cleaner(multilistFree) multilist *ml =
            multilistNew(FLEX_CAP_LEVEL_512, 0);
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();

        persist *p = persistCreate(&persistOpsMultilist, NULL);
        persistAttachSnapshot(p, persistStoreFile(snapPath, true));
        persistAttachWAL(p, persistStoreFile(walPath, true));

        for (int i = 0; i < 50; i++) {
            databox box = DATABOX_SIGNED(i);
            multilistPushByTypeTail(&ml, state, &box);
            persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box, 1);
        }
        if (!persistSnapshot(p, ml)) {
            ERRR("File-backed snapshot failed");
        }

        /* The atomic publish must have renamed its temp over the live
         * file and left nothing torn behind. */
        if (access(tmpPath, F_OK) == 0) {
            ERRR("Atomic publish left a temp snapshot file behind");
        }
        if (access(snapPath, F_OK) != 0) {
            ERRR("Published snapshot file is missing");
        }

        /* Recover: the snapshot holds all 50 ops and the un-truncated
         * WAL holds them below the watermark, so replay must skip them
         * and recover exactly 50 (crash-consistent, no double-apply). */
        multilist *recovered = persistRecover(p);
        if (!recovered || multilistCount(recovered) != 50) {
            ERR("File-backed recovery wrong: got %zu",
                recovered ? multilistCount(recovered) : 0);
        }

        if (recovered) {
            multilistFree(recovered);
        }
        persistClose(p);
        unlink(snapPath);
        unlink(walPath);
        unlink(tmpPath);
    }

    /* ============================================================
     * Adversarial restore tests for the defensive bounds checks
     * recently added to flex/multimap/multilist/multidict/multilru/
     * multiOrderedSet/intset restore paths. Each ops->restore must:
     * - return NULL on NULL input
     * - return NULL on zero/undersize input
     * - return NULL when the embedded length header doesn't match
     * the supplied buffer length
     * These tests invoke restore directly (not through the snapshot
     * file format) so we can hand it crafted malformed bytes. */
    TEST("multimap WAL insert replays a 128-bit value") {
        /* The replayed INSERT record is a flex of the row's members; a
         * 128-bit member must decode into storage the box owns. */
        cleaner(multimapFree) multimap *m = multimapNew(2);
        cleaner(flexFree) flex *rec = flexNew();
        const databox key = databoxNewBytesString("wide");
        databoxBig wide;
        DATABOX_BIG_SIGNED_128(&wide, -((__int128_t)1 << 100) - 7);
        flexPushByType(&rec, &key, FLEX_ENDPOINT_TAIL);
        flexPushByType(&rec, (const databox *)&wide, FLEX_ENDPOINT_TAIL);
        if (!persistOpsMultimap.applyOp(&m, PERSIST_OP_INSERT,
                                        (const uint8_t *)rec, flexBytes(rec))) {
            ERRR("multimap INSERT replay refused a valid 128-bit row");
        }
        databoxBig got;
        DATABOX_BIG_INIT(&got);
        databox *gotPtrs[1] = {(databox *)&got};
        if (!multimapLookup(m, &key, gotPtrs)) {
            ERRR("replayed 128-bit row not found");
        } else if (got.type != DATABOX_SIGNED_128 ||
                   *got.data.i128 != -((__int128_t)1 << 100) - 7) {
            ERRR("replayed 128-bit value read back wrong");
        }
    }

    TEST("multimap WAL delete replays for a 128-bit key") {
        cleaner(multimapFree) multimap *m = multimapNew(2);
        databoxBig wideKey;
        DATABOX_BIG_SIGNED_128(&wideKey, ((__int128_t)1 << 90) + 3);
        const databox val = DATABOX_SIGNED(7);
        const databox *row[2] = {(const databox *)&wideKey, &val};
        multimapInsert(&m, row);

        cleaner(flexFree) flex *rec = flexNew();
        flexPushByType(&rec, (const databox *)&wideKey, FLEX_ENDPOINT_TAIL);
        if (!persistOpsMultimap.applyOp(&m, PERSIST_OP_DELETE,
                                        (const uint8_t *)rec, flexBytes(rec))) {
            ERRR("multimap DELETE replay refused a 128-bit key");
        }

        if (multimapExists(m, (const databox *)&wideKey)) {
            ERRR("replayed DELETE left the 128-bit key in place");
        }
    }

    TEST("multiOrderedSet snapshot round-trips 128-bit scores and members") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();
        databoxBig wideScore, wideMember;
        DATABOX_BIG_SIGNED_128(&wideScore, -((__int128_t)1 << 100) - 7);
        DATABOX_BIG_UNSIGNED_128(&wideMember, ((__uint128_t)1 << 127) + 1);
        const databox narrowScore = DATABOX_SIGNED(5);
        const databox narrowMember = databoxNewBytesString("narrow");
        multiOrderedSetAdd(&mos, (const databox *)&wideScore,
                           (const databox *)&wideMember);
        multiOrderedSetAdd(&mos, &narrowScore, &narrowMember);

        persist *p = persistCreate(&persistOpsMultiOrderedSet, NULL);
        persistAttachSnapshot(p, persistStoreMemory(0));
        if (!persistSnapshot(p, mos)) {
            ERRR("multiOrderedSet snapshot with 128-bit values failed");
        }

        cleaner(multiOrderedSetFree) multiOrderedSet *restored =
            persistRestore(p);
        databoxBig got;
        DATABOX_BIG_INIT(&got);
        if (!restored || multiOrderedSetCount(restored) != 2) {
            ERRR("restored multiOrderedSet lost entries");
        } else if (!multiOrderedSetGetScore(restored,
                                            (const databox *)&wideMember,
                                            (databox *)&got) ||
                   got.type != DATABOX_SIGNED_128 ||
                   *got.data.i128 != -((__int128_t)1 << 100) - 7) {
            ERRR("restored 128-bit score read back wrong");
        }

        persistClose(p);
    }

    TEST("restore: NULL input is rejected by all flex-backed ops") {
        if (persistOpsFlex.restore(NULL, 0) != NULL) {
            ERRR("flex restore accepted NULL input");
        }
        if (persistOpsFlex.restore(NULL, 1024) != NULL) {
            ERRR("flex restore accepted NULL with nonzero len");
        }
        if (persistOpsMultimap.restore(NULL, 0) != NULL) {
            ERRR("multimap restore accepted NULL input");
        }
        if (persistOpsMultilist.restore(NULL, 0) != NULL) {
            ERRR("multilist restore accepted NULL input");
        }
        if (persistOpsMultidict.restore(NULL, 0) != NULL) {
            ERRR("multidict restore accepted NULL input");
        }
        if (persistOpsIntset.restore(NULL, 0) != NULL) {
            ERRR("intset restore accepted NULL input");
        }
        if (persistOpsIntsetBig.restore(NULL, 0) != NULL) {
            ERRR("intsetBig restore accepted NULL input");
        }
        if (persistOpsHyperloglog.restore(NULL, 0) != NULL) {
            ERRR("hyperloglog restore accepted NULL input");
        }
        if (persistOpsMultiroar.restore(NULL, 0) != NULL) {
            ERRR("multiroar restore accepted NULL input");
        }
    }

    TEST("restore: truncated flex header is rejected") {
        /* Anything under FLEX_EMPTY_HEADER_SIZE cannot be a valid
         * flex envelope. Verify the flex-backed ops bail without
         * touching the bytes past the end. */
        const uint8_t tiny[1] = {0xff};
        if (persistOpsFlex.restore(tiny, 0) != NULL) {
            ERRR("flex restore accepted zero-length buffer");
        }
        if (persistOpsFlex.restore(tiny, 1) != NULL) {
            ERRR("flex restore accepted 1-byte buffer");
        }
        if (persistOpsMultilist.restore(tiny, 1) != NULL) {
            ERRR("multilist restore accepted 1-byte buffer");
        }
        if (persistOpsMultidict.restore(tiny, 1) != NULL) {
            ERRR("multidict restore accepted 1-byte buffer");
        }
    }

    TEST("restore: flex with mismatched total_bytes is rejected") {
        /* Build a valid flex, then claim a wrong length to the
         * restore function. Both too-short and too-long must be
         * rejected because the header self-describes the length. */
        cleaner(flexFree) flex *valid = flexNew();
        flexPushSigned(&valid, 1, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&valid, 2, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&valid, 3, FLEX_ENDPOINT_TAIL);
        const size_t real = flexBytes(valid);

        if (persistOpsFlex.restore((uint8_t *)valid, real - 1) != NULL) {
            ERRR("flex restore accepted buffer 1 byte shorter than "
                 "header claims");
        }
        if (persistOpsFlex.restore((uint8_t *)valid, real + 1) != NULL) {
            ERRR("flex restore accepted buffer 1 byte longer than "
                 "header claims");
        }
        /* Real length must round-trip cleanly. */
        flex *ok = persistOpsFlex.restore((uint8_t *)valid, real);
        if (!ok) {
            ERRR("flex restore rejected a valid buffer");
        } else {
            flexFree(ok);
        }
    }

    TEST("restore: intset count*sizeof overflow is rejected") {
        /* Header claims a count that, when multiplied by sizeof(int64_t),
         * would overflow size_t. Without the overflow guard, the
         * `expectedLen = sizeof(uint64_t) + count*8` arithmetic wraps
         * and the underflow check passes — letting subsequent reads
         * walk off the end. */
        uint8_t buf[16];
        const uint64_t overflowing = (SIZE_MAX / 8) + 1;
        memcpy(buf, &overflowing, sizeof(uint64_t));
        memset(buf + 8, 0, 8);
        if (persistOpsIntset.restore(buf, sizeof(buf)) != NULL) {
            ERRR("intset restore accepted count that overflows "
                 "size_t when multiplied by sizeof(int64_t)");
        }
    }

    TEST("restore: hyperloglog rejects empty input but accepts valid") {
        if (persistOpsHyperloglog.restore(NULL, 0) != NULL) {
            ERRR("hyperloglog restore accepted NULL");
        }

        /* Round-trip: build a real HLL, snapshot it, restore. */
        cleaner(hyperloglogFree) hyperloglog *h = hyperloglogNew();
        hyperloglogAdd(&h, "abc", 3);
        hyperloglogAdd(&h, "def", 3);
        size_t snapLen = 0;
        uint8_t *snap = persistOpsHyperloglog.snapshot(h, &snapLen);
        assert(snap != NULL && snapLen > 0);
        hyperloglog *restored = persistOpsHyperloglog.restore(snap, snapLen);
        if (!restored) {
            ERRR("hyperloglog restore rejected its own snapshot");
        } else {
            persistOpsHyperloglog.free(restored);
        }
        zfree(snap);
    }

    TEST("restore: intsetBig rejects malformed and round-trips valid") {
        if (persistOpsIntsetBig.restore(NULL, 0) != NULL) {
            ERRR("intsetBig restore accepted NULL");
        }
        const uint8_t tooShort[4] = {1, 0, 0, 0};
        if (persistOpsIntsetBig.restore(tooShort, sizeof(tooShort)) != NULL) {
            ERRR("intsetBig restore accepted buffer shorter than count "
                 "header");
        }

        /* Round-trip: build a real intsetBig, snapshot, restore,
         * count must match. */
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        databoxBig dbig = {0};
        DATABOX_BIG_INIT(&dbig);
        dbig.type = DATABOX_SIGNED_64;
        dbig.data.i = -42;
        intsetBigAdd(isb, &dbig);
        dbig.data.i = 7;
        intsetBigAdd(isb, &dbig);
        dbig.type = DATABOX_UNSIGNED_64;
        dbig.data.u = (uint64_t)1 << 63;
        intsetBigAdd(isb, &dbig);

        size_t snapLen = 0;
        uint8_t *snap = persistOpsIntsetBig.snapshot(isb, &snapLen);
        assert(snap != NULL && snapLen > 0);
        intsetBig *restored = persistOpsIntsetBig.restore(snap, snapLen);
        if (!restored) {
            ERRR("intsetBig restore rejected its own snapshot");
        } else {
            if (intsetBigCountElements(restored) !=
                intsetBigCountElements(isb)) {
                ERR("intsetBig round-trip count mismatch: %zu vs %zu",
                    intsetBigCountElements(restored),
                    intsetBigCountElements(isb));
            }
            persistOpsIntsetBig.free(restored);
        }
        zfree(snap);
    }

    TEST("intsetBig round trip: 100 members, half beyond 64 bits") {
        /* The snapshot appends members in chunks, each wide value in its
         * own slot: past several chunks the set must come back equal. */
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        for (int64_t i = 0; i < 100; i++) {
            databoxBig v;
            if (i % 2) {
                const __int128_t wide = ((__int128_t)1 << 70) + i;
                DATABOX_BIG_SIGNED_128(&v, i % 4 == 1 ? wide : -wide);
            } else {
                memset(&v, 0, sizeof(v));
                v.type = DATABOX_SIGNED_64;
                v.data.i = i * 1000 - 50000;
            }

            intsetBigAdd(isb, &v);
        }

        size_t snapLen = 0;
        uint8_t *snap = persistOpsIntsetBig.snapshot(isb, &snapLen);
        intsetBig *restored =
            snap ? persistOpsIntsetBig.restore(snap, snapLen) : NULL;
        if (!restored || intsetBigCountElements(restored) != 100 ||
            !intsetBigEqual(restored, isb)) {
            ERRR("a 100-member intsetBig did not round-trip equal");
        }

        if (restored) {
            persistOpsIntsetBig.free(restored);
        }

        zfree(snap);
    }

    TEST("restore: newEmpty present where WAL-replay-from-bare needs it") {
        /* The newEmpty hook lets restore engines apply a delta WAL
         * that begins without a snapshot. Verify the hook is wired
         * for the ops that intend to support that flow. */
        if (!persistOpsIntsetBig.newEmpty) {
            ERRR("persistOpsIntsetBig.newEmpty is NULL");
        }
        if (!persistOpsHyperloglog.newEmpty) {
            ERRR("persistOpsHyperloglog.newEmpty is NULL");
        }
        if (!persistOpsMultiroar.newEmpty) {
            ERRR("persistOpsMultiroar.newEmpty is NULL");
        }
        if (!persistOpsMultimap.newEmpty) {
            ERRR("persistOpsMultimap.newEmpty is NULL");
        }
        if (!persistOpsMultilist.newEmpty) {
            ERRR("persistOpsMultilist.newEmpty is NULL");
        }
        /* Spot-check that newEmpty returns a usable object. */
        void *empty = persistOpsIntsetBig.newEmpty();
        if (!empty) {
            ERRR("persistOpsIntsetBig.newEmpty returned NULL");
        } else {
            persistOpsIntsetBig.free(empty);
        }
        empty = persistOpsHyperloglog.newEmpty();
        if (!empty) {
            ERRR("persistOpsHyperloglog.newEmpty returned NULL");
        } else {
            persistOpsHyperloglog.free(empty);
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
