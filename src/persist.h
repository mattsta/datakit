#pragma once

/* ============================================================================
 * PERSIST - Pluggable Persistence Framework for Linear Data Structures
 * ============================================================================
 *
 * A clean abstraction for persisting datakit's memory-efficient data structures
 * to disk with support for:
 * - Full snapshots (point-in-time serialization)
 * - Write-Ahead Log (incremental operations)
 * - Compaction (merge WAL into snapshot)
 * - Crash recovery (snapshot + WAL replay)
 *
 * Architecture Overview:
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │ Application Layer │
 * │ multimap, multilist, multidict, intset, multiOrderedSet, etc. │
 * └─────────────────────────────────────────────────────────────────────────┘
 * │
 * implements persistOps interface
 * ▼
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │ Persistence Abstraction │
 * │ │
 * │ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ │
 * │ │ Snapshot │ │ WAL │ │ Compaction │ │
 * │ │ Manager │ │ Manager │ │ Engine │ │
 * │ └──────────────┘ └──────────────┘ └──────────────┘ │
 * │ │ │ │ │
 * │ └───────────────────┴───────────────────┘ │
 * │ │ │
 * │ persistStore interface │
 * └─────────────────────────────────────────────────────────────────────────┘
 * │
 * ▼
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │ Storage Backends │
 * │ │
 * │ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ │
 * │ │ File │ │ Memory │ │ Custom │ │
 * │ │ Backend │ │ Backend │ │ Backend │ │
 * │ └──────────────┘ └──────────────┘ └──────────────┘ │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * On-Disk Format:
 * ───────────────
 *
 * SNAPSHOT FILE (.snap):
 * ┌────────────────────────────────────────────────────────────────┐
 * │ Header (72 bytes as written; headerLen covers fields a later │
 * │ container version appends ahead of the digest) │
 * │ magic[4] = "DKSP" │
 * │ headerLen[4] = header bytes, digest included │
 * │ containerVersion[4] = file framing version (dkFormat stamp) │
 * │ formatId[4] = the structure's format id (its persistType) │
 * │ containerFeatures[8] = file framing feature bits │
 * │ formatVersion[4] = the structure format's version │
 * │ keyId[4] = attached keyring key_id (0 if no keyring) │
 * │ formatFeatures[8] = the structure format's feature bits │
 * │ count[8] = element count │
 * │ envelopeLen[8] = size of the body envelope in bytes │
 * │ walSequence[8] = WAL sequence the snapshot includes │
 * │ digest[8] = XXH64 over the header's other bytes │
 * ├────────────────────────────────────────────────────────────────┤
 * │ Body envelope (envelopeLen bytes) │
 * │ flexEnvelopeWrap(serialized payload, configured algorithm). │
 * │ Carries its own magic, length, algorithm id, and digest; │
 * │ keyed algorithms (HMAC_*, BLAKE3_KEYED) require a key set │
 * │ via persistSetIntegrityKey before snapshot/restore. │
 * └────────────────────────────────────────────────────────────────┘
 *
 * The `keyId` field lets a reader detect "operator pointed me at the
 * wrong key" early: on verify, if a keyring is attached AND the on-
 * disk keyId is non-zero AND it differs from the attached key_id, the
 * verify fails with PERSIST_ERR_KEY_ID_MISMATCH (use
 * persistLastExpectedKeyId to recover the expected id) rather than
 * the opaque generic-VERIFY_FAILED an HMAC mismatch would produce.
 *
 * WAL FILE (.wal):
 * ┌────────────────────────────────────────────────────────────────┐
 * │ WAL Header (56 bytes as written; growable like the snapshot's) │
 * │ magic[4] = "DKWL" │
 * │ headerLen[4], containerVersion[4], formatId[4], │
 * │ containerFeatures[8], formatVersion[4], keyId[4], │
 * │ formatFeatures[8] = as in the snapshot header; every entry │
 * │ in the file is in this structure format │
 * │ sequence[8] = starting sequence number │
 * │ digest[8] = XXH64 over the header's other bytes │
 * ├────────────────────────────────────────────────────────────────┤
 * │ Entry 0 │
 * │ envelopeLen[4] = byte length of the entry envelope │
 * │ envelope[envLen] = flexEnvelopeWrap(seq||op||opdata, algo). │
 * │ seq is 8 bytes, op is 1 byte, then │
 * │ persistOps.encodeOp output. │
 * ├────────────────────────────────────────────────────────────────┤
 * │ Entry 1...N │
 * └────────────────────────────────────────────────────────────────┘
 *
 * A reader refuses a file whose container or structure stamp it does not
 * read (dkFormatCheck: version outside its range, or a must-understand
 * feature it lacks) with PERSIST_ERR_CONTAINER_FORMAT /
 * PERSIST_ERR_STRUCTURE_FORMAT; persistInspect reports the stamps without
 * reading the body.  A WAL this build refuses is never appended to or
 * overwritten.
 *
 * The header digest is a cheap consistency
 * tag — its only job is to reject obviously-corrupt files before the
 * envelope verify runs. The envelope is the source of truth for body /
 * entry integrity.
 *
 * ============================================================================
 */

#include "dkFormat.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Forward declarations */
typedef struct persist persist;
typedef struct persistStore persistStore;
typedef struct persistOps persistOps;
typedef struct persistConfig persistConfig;
typedef struct persistStats persistStats;
typedef struct persistWALEntry persistWALEntry;

/* ============================================================================
 * Structure Type Registry
 * ============================================================================
 * Each persistable data structure has a unique type ID.
 * IDs are stable and must never change (on-disk format compatibility).
 */
typedef enum persistType {
    PERSIST_TYPE_INVALID = 0,

    /* Core linear structures */
    PERSIST_TYPE_FLEX = 1,
    PERSIST_TYPE_INTSET = 2,
    PERSIST_TYPE_INTSETBIG = 4, /* signed/unsigned 64/128 set */

    /* Multi-tier structures */
    PERSIST_TYPE_MULTILIST = 10,
    PERSIST_TYPE_MULTILIST_SMALL = 11,
    PERSIST_TYPE_MULTILIST_MEDIUM = 12,
    PERSIST_TYPE_MULTILIST_FULL = 13,

    PERSIST_TYPE_MULTIMAP = 20,
    PERSIST_TYPE_MULTIMAP_SMALL = 21,
    PERSIST_TYPE_MULTIMAP_MEDIUM = 22,
    PERSIST_TYPE_MULTIMAP_FULL = 23,

    PERSIST_TYPE_MULTIDICT = 30,

    PERSIST_TYPE_MULTIARRAY = 40,
    PERSIST_TYPE_MULTIARRAY_SMALL = 41,
    PERSIST_TYPE_MULTIARRAY_MEDIUM = 42,
    PERSIST_TYPE_MULTIARRAY_LARGE = 43,

    PERSIST_TYPE_MULTIORDEREDSET = 50,
    PERSIST_TYPE_MULTILRU = 60,
    PERSIST_TYPE_MULTIROAR = 70,

    /* Probabilistic structures */
    PERSIST_TYPE_LINEARBLOOM = 80,
    PERSIST_TYPE_HYPERLOGLOG = 81,

    /* Security / crypto state */
    PERSIST_TYPE_HMAC_KEYRING = 90,
    PERSIST_TYPE_CLUSTER_MAILBOX_WATERMARKS = 91,

    /* Cluster topology */
    PERSIST_TYPE_CLUSTER_RING = 92,

    /* AEAD replay-protection state (per-key nonce log). */
    PERSIST_TYPE_AEAD_NONCE_LOG = 93,

    /* Typed Fenwick trees (fenwick/fenwickPersist.h) */
    PERSIST_TYPE_FENWICK_I16 = 100,
    PERSIST_TYPE_FENWICK_I32 = 101,
    PERSIST_TYPE_FENWICK_I64 = 102,
    PERSIST_TYPE_FENWICK_I128 = 103,
    PERSIST_TYPE_FENWICK_U16 = 104,
    PERSIST_TYPE_FENWICK_U32 = 105,
    PERSIST_TYPE_FENWICK_U64 = 106,
    PERSIST_TYPE_FENWICK_U128 = 107,
    PERSIST_TYPE_FENWICK_FLOAT = 108,
    PERSIST_TYPE_FENWICK_DOUBLE = 109,
    PERSIST_TYPE_FENWICK2D_I16 = 110,
    PERSIST_TYPE_FENWICK2D_I32 = 111,
    PERSIST_TYPE_FENWICK2D_I64 = 112,
    PERSIST_TYPE_FENWICK2D_I128 = 113,
    PERSIST_TYPE_FENWICK2D_U16 = 114,
    PERSIST_TYPE_FENWICK2D_U32 = 115,
    PERSIST_TYPE_FENWICK2D_U64 = 116,
    PERSIST_TYPE_FENWICK2D_U128 = 117,
    PERSIST_TYPE_FENWICK2D_FLOAT = 118,
    PERSIST_TYPE_FENWICK2D_DOUBLE = 119,

    /* Databox Fenwick trees */
    PERSIST_TYPE_MULTIFENWICK = 120,
    PERSIST_TYPE_MULTIFENWICK_FLAT = 121,
    PERSIST_TYPE_MULTIFENWICK_2D = 122,

    /* Typed segment trees (segment/segmentPersist.h) */
    PERSIST_TYPE_SEGMENT_I16 = 130,
    PERSIST_TYPE_SEGMENT_I32 = 131,
    PERSIST_TYPE_SEGMENT_I64 = 132,
    PERSIST_TYPE_SEGMENT_I128 = 133,
    PERSIST_TYPE_SEGMENT_U16 = 134,
    PERSIST_TYPE_SEGMENT_U32 = 135,
    PERSIST_TYPE_SEGMENT_U64 = 136,
    PERSIST_TYPE_SEGMENT_U128 = 137,
    PERSIST_TYPE_SEGMENT_FLOAT = 138,
    PERSIST_TYPE_SEGMENT_DOUBLE = 139,

    /* Atom tables (atomPersist.h) */
    PERSIST_TYPE_MULTIMAP_ATOM = 140,
    PERSIST_TYPE_MULTIMAP_ATOM_DENSE = 141,
    PERSIST_TYPE_STRING_POOL = 142,
    PERSIST_TYPE_ATOM_POOL = 143,

    /* Reserved for future use */
    PERSIST_TYPE_MAX = 255
} persistType;

/* ============================================================================
 * WAL Operation Types
 * ============================================================================
 * Generic operations that map to structure-specific implementations.
 */
typedef enum persistOp {
    PERSIST_OP_NOP = 0, /* No operation (used for alignment/padding) */

    /* Element operations */
    PERSIST_OP_INSERT = 1,  /* Insert element(s) */
    PERSIST_OP_DELETE = 2,  /* Delete element(s) */
    PERSIST_OP_UPDATE = 3,  /* Update element in place */
    PERSIST_OP_REPLACE = 4, /* Delete + Insert atomically */

    /* Positional operations */
    PERSIST_OP_PUSH_HEAD = 10, /* Push to head (list-like) */
    PERSIST_OP_PUSH_TAIL = 11, /* Push to tail */
    PERSIST_OP_POP_HEAD = 12,  /* Pop from head */
    PERSIST_OP_POP_TAIL = 13,  /* Pop from tail */
    PERSIST_OP_INSERT_AT = 14, /* Insert at index */
    PERSIST_OP_DELETE_AT = 15, /* Delete at index */
    PERSIST_OP_TRIM = 16,      /* List trim; operand is the NORMALIZED
                                * [head_cut, tail_cut] element counts
                                * (2-elem flex) so replay applies the
                                * exact effect the origin computed */
    PERSIST_OP_ROTATE = 17,    /* Same-list tail->head rotate; no
                                * operand.  NOT idempotent — WAL-local
                                * exactly-once replay only */

    /* Bulk operations */
    PERSIST_OP_CLEAR = 20,       /* Remove all elements */
    PERSIST_OP_BULK_INSERT = 21, /* Insert multiple elements */
    PERSIST_OP_BULK_DELETE = 22, /* Delete multiple elements */
    PERSIST_OP_MERGE = 23,       /* Merge another structure */

    /* Range operations (operand: two raw uint64_t — [start, extent]).
     * Fixed-width like the single-position bitmap ops, so no flex
     * framing.  SET_RANGE and CLEAR_RANGE are idempotent; FLIP_RANGE is
     * NOT (applying it twice un-flips), so it must never be re-folded on
     * a replica without executor-identity dedupe. */
    PERSIST_OP_CLEAR_RANGE = 24, /* Clear all bits in [start, start+extent) */
    PERSIST_OP_FLIP_RANGE = 25,  /* Flip all bits in [start, start+extent) */
    PERSIST_OP_SET_RANGE = 33,   /* Set all bits in [start, start+extent) */

    /* Numeric scalar deltas (operand: 1-element flex databox).
     * The row carries the INCREMENT, never the absolute result, so
     * concurrent increments from different origins fold correctly on
     * every replica instead of last-writer-wins clobbering. */
    /* String concatenation deltas (operand: RAW appended/prepended
     * bytes, no framing — the payload IS the operand).  Emitters only
     * choose these when the existing value is LONGER than the operand
     * (else a REPLACE snapshot is strictly cheaper end-to-end). */
    PERSIST_OP_STR_APPEND = 26,  /* value = value . operand */
    PERSIST_OP_STR_PREPEND = 27, /* value = operand . value */

    PERSIST_OP_NUM_INCR = 28,    /* Add operand to current value */
    PERSIST_OP_NUM_DECR_F0 = 29, /* Subtract operand, floor at 0 */

    /* Concat inverses (operand: raw uint64_t byte count).  Emitted
     * ONLY by the pending-RMW ledger's truncation un-fold — a
     * speculative STR_APPEND/STR_PREPEND fold that consensus
     * truncated is reversed by cutting the same byte count off the
     * same end.  Never written to a durable WAL row. */
    PERSIST_OP_STR_TRUNC_TAIL = 30, /* drop last N bytes */
    PERSIST_OP_STR_TRUNC_HEAD = 31, /* drop first N bytes */

    /* Opaque whole-value write for host-defined (plugin) types.
     * Operand shape is identical to a STRING snapshot
     * ([encoded_type_byte][encoded_bytes]) because the value IS opaque
     * bytes to everything below the plugin that produced it.
     *
     * Distinct from PERSIST_OP_REPLACE despite carrying a full value:
     * REPLACE is the ABSOLUTE class (idempotent, unstamped, position-
     * gated by the install frontier), while a plugin write is produced
     * by executing host code against the value the executor read.  Only
     * ONE node ran that code, so the row must carry executor identity
     * and be suppressed on the executor's own re-apply — the same
     * discipline the numeric deltas use.  Sharing REPLACE's op code
     * would make those two contracts indistinguishable on the wire. */
    PERSIST_OP_OPAQUE_SET = 32,

    /* Structure-specific operations (encoded in data) */
    PERSIST_OP_CUSTOM = 100,

    PERSIST_OP_MAX = 255
} persistOp;

/* For a store whose rows each hold one whole structure (an embedding
 * key-value layer folding a WAL per key): does this row's op carry a
 * COMPLETE serialized value rather than a delta?  The store's replay,
 * compaction and restore folds ask this instead of naming REPLACE
 * directly: such a row RESETS the structure (restore the bytes, discard
 * whatever was folded before it) and is not routed through applyOp.
 * (Inside one structure's own WAL, REPLACE is an element verb that
 * descriptors such as multidict's and multilist's replay through
 * applyOp; this predicate is about the store's rows, not those.)
 *
 * Two ops answer yes and they are NOT interchangeable on the wire —
 * REPLACE is the absolute, unstamped, anyone-could-have-produced-it
 * class, while OPAQUE_SET is the stamped output of host code that ran
 * on exactly one node.  They agree only about operand SHAPE, which is
 * precisely and only what this predicate asks about. */
static inline bool persistOpCarriesFullValue(uint16_t op) {
    return op == (uint16_t)PERSIST_OP_REPLACE ||
           op == (uint16_t)PERSIST_OP_OPAQUE_SET;
}

/* ============================================================================
 * Checksum Algorithms
 * ============================================================================
 * Snapshot bodies and WAL entries are wrapped in a flexEnvelope; the
 * algorithm below names which digest the envelope carries. Algorithms
 * fall into two classes:
 *
 * - Checksum class (XXH64, XXH3_64, XXH3_128) — fast integrity, no key.
 * Good for local on-disk persistence on a trusted host.
 *
 * - MAC class (BLAKE3_KEYED, HMAC_SHA256, HMAC_BLAKE3) — tamper-evident,
 * requires a key set via persistSetIntegrityKey. Use these for
 * cross-host replication, plugin ingest, or anywhere the bytes can be
 * forged by an attacker between writer and reader.
 *
 * The persist header itself carries a small XXH64/XXH32 self-check so
 * obviously-corrupt files fail fast without invoking the envelope work.
 */
/* Snapshot bodies and WAL entries are always wrapped in a flexEnvelope.
 * The envelope carries the algorithm id and digest internally, so the
 * persist header no longer encodes a checksum type. Keyed algorithms
 * (HMAC_SHA256, HMAC_BLAKE3, BLAKE3_KEYED) require a key supplied via
 * persistSetIntegrityKey before snapshot/log; writes are refused if no
 * key is set. */
typedef enum persistChecksum {
    PERSIST_CHECKSUM_NONE =
        0,                             /* 0 B digest, test-only (refused
                                                  unless built with
                                                  -DDATAKIT_ENABLE_INSECURE_TEST_ALGOS) */
    PERSIST_CHECKSUM_XXH64 = 1,        /* 8 B, no key */
    PERSIST_CHECKSUM_XXH3_64 = 2,      /* 8 B, no key */
    PERSIST_CHECKSUM_XXH3_128 = 3,     /* 16 B, no key */
    PERSIST_CHECKSUM_BLAKE3_KEYED = 4, /* 32 B, keyed (native BLAKE3) */
    PERSIST_CHECKSUM_HMAC_SHA256 = 5,  /* 32 B, keyed (RFC 4231) */
    PERSIST_CHECKSUM_HMAC_BLAKE3 = 6,  /* 32 B, keyed (RFC 2104 over BLAKE3) */
} persistChecksum;

/* True iff `t` is a keyed (tamper-evident) algorithm that requires a key. */
#define PERSIST_CHECKSUM_IS_KEYED(t)                                           \
    ((t) == PERSIST_CHECKSUM_HMAC_SHA256 ||                                    \
     (t) == PERSIST_CHECKSUM_HMAC_BLAKE3 ||                                    \
     (t) == PERSIST_CHECKSUM_BLAKE3_KEYED)

/* ============================================================================
 * Persistence Operations Interface
 * ============================================================================
 * Each data structure implements this interface to be persistable.
 * The framework is structure-agnostic; all specifics are in these callbacks.
 *
 * Every descriptor meets one contract, checked by persistContractTest
 * (src/persist/contract/) at several sizes and seeds in every build
 * configuration:
 *   1. snapshot -> restore -> snapshot gives identical bytes, an equal
 *      count and validate() true; the empty structure included.
 *   2. For a descriptor with a WAL (encodeOp and applyOp): ops recorded
 *      through encodeOp while a structure is built, applied through
 *      applyOp to the restored starting structure (and to
 *      newEmpty() when that is the start, and to a restored mid-point
 *      snapshot for the remaining ops), give the live snapshot bytes.
 *   3. Every truncation and single-byte corruption of a snapshot makes
 *      restore return NULL or a structure that itself meets (1); never a
 *      crash.  Restore is where untrusted bytes enter.
 *   4. Strings that look like numbers ("22", "007", "1.5", 128-bit
 *      numerals) stay the same bytes through snapshot, restore and
 *      replay: persistence stores values exactly as given.
 */
struct persistOps {
    /* Structure identification */
    persistType type;
    const char *name; /* Human-readable name, e.g., "multimap" */

    /* The structure's byte format (its snapshot payload and WAL op
     * encodings; see dkFormat.h): the version this build writes, the
     * oldest it reads and the feature bits it understands.  persist files
     * record the format's stamp, and the persist layer refuses a stamp
     * outside these before restore or applyOp see any bytes.  A
     * descriptor must set formatVersion (persistCreate refuses 0). */
    uint32_t formatVersion;
    uint32_t minReadVersion;
    uint64_t knownFeatures;

    /* ---- Snapshot Operations ---- */

    /* Serialize entire structure to buffer.
     * Returns allocated buffer (caller frees), sets *len.
     * Returns NULL on error. */
    uint8_t *(*snapshot)(const void *structure, size_t *len);

    /* Deserialize buffer to structure.
     * Returns allocated structure (caller frees).
     * Returns NULL on error.
     *
     * ALIGNMENT CONTRACT (important): `data` may point to ANY byte
     * offset inside a larger envelope buffer — specifically, it sits
     * just after a variable-length varint framing header, so the
     * pointer is NOT guaranteed to be aligned for any multi-byte
     * type. Restore implementations MUST use `memcpy` when reading
     * fields wider than 1 byte (e.g.
     * uint64_t count;
     * memcpy(&count, data, sizeof(count));
     * ). Pointer-cast reads such as `*(const uint64_t *)data` will
     * trip UBSAN on every modern toolchain and may produce a bus
     * fault on stricter platforms. On the targets where unaligned
     * loads ARE permitted, memcpy compiles to a single load — there
     * is no runtime cost. */
    void *(*restore)(const uint8_t *data, size_t len);

    /* Allocate a fresh, empty structure of this type — the moral
     * equivalent of "calling the structure's plain constructor".
     * Used by restore engines that need to apply a delta WAL whose
     * leading op is not a snapshot (the structure's identity is
     * defined only by its leading newEmpty + applied deltas).
     * Caller frees via .free. May be NULL for types that have no
     * meaningful empty state; restore engines treat NULL as "skip
     * this key" with a diagnostic. */
    void *(*newEmpty)(void);

    /* Get current element count (for header) */
    size_t (*count)(const void *structure);

    /* Approximate serialized size, a pre-allocation hint: most types
     * return their in-memory size, which can be below the snapshot's.
     * Never size a buffer a snapshot must fit with it. */
    size_t (*estimateSize)(const void *structure);

    /* ---- WAL Operations ---- */

    /* Encode a single operation to buffer.
     * op: operation type
     * args: operation-specific arguments (databox array or structure-specific)
     * argc: number of arguments
     * Returns allocated buffer, sets *len. NULL on error. */
    uint8_t *(*encodeOp)(persistOp op, const void *args, size_t argc,
                         size_t *len);

    /* Decode and apply a single operation.
     * structure: the ADDRESS of the structure pointer (a void ** passed
     *            as void *), so the op may replace the structure (a flex
     *            reallocates); the other hooks take the pointer itself
     * op: operation type
     * data: operation data from WAL
     * len: data length
     * Returns true on success. */
    bool (*applyOp)(void *structure, persistOp op, const uint8_t *data,
                    size_t len);
    /* True when an empty instance of this type is not a meaningful
     * value, so an embedding application may delete a key when its last
     * element is removed.  Advisory: the persist layer does not read
     * it, and false only means the ops table gives no such advice, not
     * that an empty instance must be kept.  Of the built-in tables,
     * persistOpsIntsetBig, persistOpsMultimap, and persistOpsMultilist
     * set it; every other built-in table leaves it false, so a
     * consistent per-type policy (for example, treating intset like
     * intsetBig) belongs to the embedding application.  The field's
     * place in the struct, after applyOp, is part of the layout. */
    bool deletesWhenEmpty;

    /* ---- Optional Optimization Hooks ---- */

    /* Stream snapshot incrementally (for very large structures).
     * If NULL, snapshot is used instead.
     * Callback is called with chunks of data.
     * Returns true on success. */
    bool (*streamSnapshot)(const void *structure,
                           bool (*emit)(const uint8_t *chunk, size_t len,
                                        void *ctx),
                           void *ctx);

    /* Incremental restore (for very large structures).
     * If NULL, restore is used instead. */
    void *(*streamRestore)(bool (*read)(uint8_t *buf, size_t len, void *ctx),
                           void *ctx);

    /* Validate structure integrity after restore.
     * Returns true if structure is valid. */
    bool (*validate)(const void *structure);

    /* Free structure (if restore allocates) */
    void (*free)(void *structure);

    /* ---- Optional Per-Fold Context Hooks ----
     *
     * Some codecs need per-thread context beyond the bytes (for
     * example, a codec that resolves members through a shared atom
     * table must serialize against concurrent writers).  A fold
     * engine that is about to call this type's
     * restore/applyOp/snapshot/free MUST bracket the calls:
     *
     *     void *tok = ops->foldEnter ? ops->foldEnter() : NULL;
     *     ... restore/applyOp/snapshot/free ...
     *     if (ops->foldExit) ops->foldExit(tok);
     *
     * foldEnter acquires whatever locks/thread-locals the codec
     * needs and returns an opaque token; foldExit releases them.
     * Both NULL for codecs with no ambient context (the common
     * case).  Hooks must be cheap and reentrancy-safe within one
     * thread (an engine may bracket per-row or per-tail). */
    void *(*foldEnter)(void);
    void (*foldExit)(void *token);
};

/* ============================================================================
 * Storage Backend Interface
 * ============================================================================
 * Abstraction over actual storage (file, memory, network, etc.)
 */
struct persistStore {
    void *ctx; /* Backend-specific context */

    /* Write data at current position, return bytes written or -1 on error */
    ssize_t (*write)(void *ctx, const void *data, size_t len);

    /* Read data at current position, return bytes read or -1 on error */
    ssize_t (*read)(void *ctx, void *buf, size_t len);

    /* Seek to position, return new position or -1 on error */
    int64_t (*seek)(void *ctx, int64_t offset, int whence);

    /* Get current position */
    int64_t (*tell)(void *ctx);

    /* Sync to durable storage */
    bool (*sync)(void *ctx);

    /* Truncate at current position */
    bool (*truncate)(void *ctx);

    /* Get total size */
    int64_t (*size)(void *ctx);

    /* Close and free resources */
    void (*close)(void *ctx);

    /* Optional atomic full-rewrite protocol for whole-file publishers
     * (the snapshot store). When all three are present, a writer that
     * replaces the entire file brackets its writes with begin/commit so
     * a crash can never expose a torn file: begin redirects subsequent
     * write/seek/truncate to a private temp, commit durably swaps the
     * temp over the live file (fsync temp, atomic rename, fsync dir),
     * and abort discards the temp leaving the live file untouched.
     * NULL for backends without crash exposure (e.g. memory), which
     * fall back to in-place overwrite. */
    bool (*rewriteBegin)(void *ctx);
    bool (*rewriteCommit)(void *ctx);
    void (*rewriteAbort)(void *ctx);
};

/* ============================================================================
 * Configuration
 * ============================================================================
 */
typedef enum persistCompression {
    PERSIST_COMPRESS_NONE = 0,
    PERSIST_COMPRESS_LZ4 = 1,   /* Fast compression */
    PERSIST_COMPRESS_ZSTD = 2,  /* High ratio compression */
    PERSIST_COMPRESS_CFLEX = 3, /* Use built-in cflex compression */
} persistCompression;

typedef enum persistSyncMode {
    PERSIST_SYNC_NONE = 0,     /* No sync (fastest, least safe) */
    PERSIST_SYNC_EVERYSEC = 1, /* Sync every second */
    PERSIST_SYNC_ALWAYS = 2,   /* Sync after every write (safest) */
} persistSyncMode;

struct persistConfig {
    /* Compression settings */
    persistCompression compression;
    int compressionLevel; /* 0 = default, higher = more compression */

    /* Checksum settings */
    persistChecksum checksumType;

    /* WAL settings */
    persistSyncMode syncMode;
    size_t walMaxSize;    /* Trigger compaction when WAL exceeds this */
    size_t walBufferSize; /* Write buffer size (default: 64KB) */

    /* Compaction settings */
    bool autoCompact;    /* Compact automatically when threshold hit */
    double compactRatio; /* Compact when WAL size > snapshot * ratio */

    /* Recovery settings */
    bool strictRecovery; /* Fail on any corruption vs skip bad entries */
};

/* ============================================================================
 * Statistics
 * ============================================================================
 */
struct persistStats {
    /* Snapshot stats */
    uint64_t snapshotCount;        /* Number of snapshots taken */
    uint64_t snapshotBytes;        /* Total bytes written in snapshots */
    uint64_t lastSnapshotTime;     /* Timestamp of last snapshot */
    uint64_t lastSnapshotDuration; /* Duration in microseconds */

    /* WAL stats */
    uint64_t walEntries;  /* Total WAL entries written */
    uint64_t walBytes;    /* Total WAL bytes */
    uint64_t walSequence; /* Current sequence number */

    /* Sync stats (persistSync and the syncs syncMode makes) */
    uint64_t syncCount;    /* syncs that reached stable storage */
    uint64_t syncFailures; /* flushes or syncs that failed */
    int32_t lastSyncError; /* errno of the latest sync when it failed; 0
                              once a later sync succeeds */

    /* Compaction stats */
    uint64_t compactionCount; /* Number of compactions */
    uint64_t lastCompactionTime;

    /* Recovery stats */
    uint64_t recoveryCount;    /* Number of recoveries */
    uint64_t entriesRecovered; /* Entries recovered in last recovery */
    uint64_t entriesSkipped;   /* Corrupt entries skipped */
};

/* ============================================================================
 * WAL Entry (for iteration)
 * ============================================================================
 */
struct persistWALEntry {
    uint64_t sequence;
    persistOp op;
    const uint8_t *data;
    size_t len;
};

/* ============================================================================
 * Core API
 * ============================================================================
 */

/* Create persistence context for a data structure */
persist *persistCreate(const persistOps *ops, const persistConfig *config);

/* Attach storage backends (snapshot and WAL can be different) */
bool persistAttachSnapshot(persist *p, persistStore *store);
bool persistAttachWAL(persist *p, persistStore *store);

/* The attached stores (NULL when none) */
persistStore *persistSnapshotStore(const persist *p);
persistStore *persistWALStore(const persist *p);

/* Configuration.  A change of walBufferSize first writes out the buffered
 * WAL bytes; when that write fails persistSetConfig returns false and
 * keeps the old configuration. */
persistConfig persistDefaultConfig(void);
bool persistSetConfig(persist *p, const persistConfig *config);
void persistGetConfig(const persist *p, persistConfig *config);

/* ---- Integrity key (for keyed checksum algos) ------------------------
 *
 * Required when the configured `checksumType` is HMAC_SHA256, HMAC_BLAKE3,
 * or BLAKE3_KEYED. Pointer is borrowed — the caller is responsible for
 * keeping the key bytes alive for the lifetime of `p` (typical pattern:
 * key is owned by a long-lived key-management module, persist just holds
 * a reference).
 *
 * For HMAC algos any non-empty key is accepted (RFC 2104). For
 * BLAKE3_KEYED the key MUST be exactly 32 bytes; shorter/longer keys
 * cause snapshot/log to fail.
 *
 * Passing NULL/0 clears any previously set key. Returns false if the
 * persist handle is NULL. Safe to call before any snapshot/log so that
 * the very first write is keyed. Re-keying is supported between writes
 * but does NOT re-key already-written data on disk; readers will see the
 * new key applied to subsequent reads. */
bool persistSetIntegrityKey(persist *p, const uint8_t *key, size_t key_len);

/* Borrowed prepared-key handle for HMAC-class algos. When set, every
 * verify (including the per-entry WAL replay loop, which is the canonical
 * "many messages, one key" hot path) consumes the prepared inner+outer
 * states directly and skips the two block compressions hmacInit would
 * otherwise pay per envelope. Caller owns the handle and the storage
 * it points into. Pass NULL to clear. Ignored for non-HMAC algos (the
 * envelope simply falls back to raw-key path).
 *
 * Typically used as a building block under persistAttachKeyring, which
 * pulls both the raw bytes and the prepared handle out of an hmacKeyring
 * by id. */
struct hmacKey;
bool persistSetIntegrityPreparedKey(persist *p, const struct hmacKey *prep);

/* Convenience: bind a persist instance to a (keyring, key_id) pair so
 * every snapshot / WAL / replay call resolves both the raw key bytes
 * and the prepared HMAC handle automatically.
 *
 * Lifetime contract: the binding stores `(ring, key_id, generation)`
 * captured at attach time — NOT raw pointers into keyring-owned memory.
 * Every subsequent persist operation re-resolves the key via
 * hmacKeyringGetGeneration + hmacKeyringLookup; if the generation has
 * advanced (because the key was retired, re-installed under the same
 * id, or its AEAD salt was rotated) the operation fails with the typed
 * error code PERSIST_ERR_KEY_GENERATION_MISMATCH (currently visible to
 * the caller as the operation returning false; the typed-error API
 * lives in persistLastError). Callers that intentionally rotated
 * the key MUST call persistRebindKeyring to refresh the binding.
 *
 * Returns false if the keyring doesn't contain `key_id`. The keyring
 * is borrowed — caller is responsible for keeping it alive until
 * persistClose. */
struct hmacKeyring;
bool persistAttachKeyring(persist *p, struct hmacKeyring *ring,
                          uint32_t key_id);

/* Re-bind a persist instance to a (keyring, key_id) pair AFTER an
 * intentional key rotation. Refreshes the cached generation so
 * subsequent operations succeed under the new key bytes. This is the
 * official "I know the key changed; please pick up the new one" entry
 * point — distinct from persistAttachKeyring only in intent (Attach is
 * for the first binding; Rebind documents at the call site that the
 * caller has handled rotation). Internally identical to Attach but
 * named separately so audit/log review can distinguish first-attach
 * from post-rotation rebind. Returns false if `key_id` is not
 * currently installed in `ring`. */
bool persistRebindKeyring(persist *p, struct hmacKeyring *ring,
                          uint32_t key_id);

/* Typed error codes returned via persistLastError after an operation
 * fails. Not all failure paths set a code yet (legacy code paths
 * return plain false); the codes below are the ones consulted by the
 * keyring-lifetime tests. */
typedef enum persistErr {
    PERSIST_ERR_NONE = 0,
    PERSIST_ERR_KEY_GENERATION_MISMATCH = 1, /* attached keyring slot was
                                                 rotated since attach */
    PERSIST_ERR_KEY_NOT_FOUND = 2,           /* attached id no longer present */
    PERSIST_ERR_NO_KEY_ATTACHED = 3,         /* keyed-algo op with no key */
    PERSIST_ERR_KEY_ID_MISMATCH = 4,         /* snapshot/WAL header carries a
                                                         key_id that doesn't match the
                                                         currently-attached keyring's
                                                         key_id (operator pointed the
                                                         reader at the wrong key) */
    PERSIST_ERR_CONTAINER_FORMAT = 5, /* the snapshot/WAL file framing is a
                                         version or feature set this build
                                         does not read (persistLastFormat) */
    PERSIST_ERR_STRUCTURE_FORMAT = 6, /* the structure's format stamp in the
                                         file is one this build does not
                                         read (persistLastFormat) */
} persistErr;

/* Read the typed error code from the most recent failed operation on
 * `p`. Returns PERSIST_ERR_NONE if no error code has been set since
 * the last successful operation. Safe to call after any return-false
 * persist API; codes are sticky until the next call that explicitly
 * clears them. Returns PERSIST_ERR_NONE for NULL `p`. */
persistErr persistLastError(const persist *p);

/* After a failed snapshot/WAL verify that set PERSIST_ERR_KEY_ID_MISMATCH,
 * returns the `key_id` recorded in the on-disk header. The operator can
 * use this value to rotate the reader's keyring back to the expected
 * key. Returns 0 if no key_id mismatch has been observed since the
 * last successful operation, or if `p` is NULL.
 *
 * The value is sticky in the same way as persistLastError — it
 * remains valid until the next operation that clears the error state.
 *
 * Typical operator workflow on a cross-key mistake:
 *
 * if (!persistVerifySnapshot(p) &&
 * persistLastError(p) == PERSIST_ERR_KEY_ID_MISMATCH) {
 * uint32_t expected = persistLastExpectedKeyId(p);
 * fprintf(stderr,
 * "snapshot expects key_id=%u; reader currently bound to "
 * "a different key — rebind via persistRebindKeyring\n",
 * (unsigned)expected);
 * }
 */
uint32_t persistLastExpectedKeyId(const persist *p);

/* After a failure that set PERSIST_ERR_CONTAINER_FORMAT or
 * PERSIST_ERR_STRUCTURE_FORMAT: why the stamp was refused, with the stamp
 * found in *found (when non-NULL).  DK_FORMAT_OK otherwise. */
dkFormatResult persistLastFormat(const persist *p, dkFormatStamp *found);

/* ---- Formats and inspection ---- */

/* The persist file framings (layer below every structure's own format) */
extern const dkFormatSpec persistFormatSnapshotFile;
extern const dkFormatSpec persistFormatWALFile;

/* The format spec a descriptor declares (id = its type) */
dkFormatSpec persistOpsFormat(const persistOps *ops);

/* What a snapshot or WAL file header says, read without decoding the body */
typedef struct persistFileInfo {
    bool wal;                /* a WAL file (else a snapshot file) */
    uint32_t headerLen;      /* header bytes, as stored */
    dkFormatStamp container; /* the file framing's stamp */
    dkFormatStamp format;    /* the structure format's stamp */
    uint32_t keyId;          /* key_id of the writer's keyring (0: none) */
    uint64_t count;          /* snapshot: element count; WAL: 0 */
    uint64_t envelopeLen;    /* snapshot: body bytes; WAL: 0 */
    uint64_t sequence;       /* snapshot: WAL sequence included;
                                WAL: sequence of its first entry */
} persistFileInfo;

/* Read the header at the start of `store` (a snapshot or a WAL; the magic
 * decides).  Returns false when it is not a readable persist header (bad
 * magic, length or digest).  Whether this build reads the file is
 * dkFormatCheck of `container` against persistFormatSnapshotFile /
 * persistFormatWALFile and of `format` against the descriptor's
 * persistOpsFormat.  Moves the store's cursor: for stores attached to a
 * live persist use persistInspectAttached. */
bool persistInspect(persistStore *store, persistFileInfo *info);

/* persistInspect of `p`'s WAL store (wal) or snapshot store */
bool persistInspectAttached(persist *p, bool wal, persistFileInfo *info);

/* ---- Snapshot Operations ---- */

/* Take a full snapshot of the structure */
bool persistSnapshot(persist *p, const void *structure);

/* Restore structure from snapshot (allocates new structure) */
void *persistRestore(persist *p);

/* ---- WAL Operations ---- */

/* What logging an operation achieved.  Only REFUSED (0, so a plain
 * truth test reads "logged") means the op is not in the WAL: apply the
 * change exactly when the result is not REFUSED.  Durability is best
 * effort: a failed flush or sync never undoes, blocks or aborts the op;
 * it reports NOT_DURABLE, counts in persistStats.syncFailures (with
 * lastSyncError), and the next sync tries again. */
typedef enum persistLogResult {
    PERSIST_LOG_REFUSED = 0,     /* not logged (encode or write failed, WAL
                                    refused): apply nothing */
    PERSIST_LOG_BUFFERED = 1,    /* logged; the sync policy writes it out
                                    later (SYNC_NONE, or EVERYSEC between
                                    syncs) */
    PERSIST_LOG_DURABLE = 2,     /* logged and synced to stable storage */
    PERSIST_LOG_NOT_DURABLE = 3, /* logged, but the sync the policy asked
                                    for failed: apply it; it is in the WAL
                                    buffer or file, durability unknown */
    PERSIST_LOG_UNCHANGED = 4,   /* (P wrappers) the op would leave the
                                    structure as it is, so nothing was
                                    logged or applied */
} persistLogResult;

/* Log an operation (appends to WAL), syncing as the config's syncMode
 * asks */
persistLogResult persistLogOp(persist *p, persistOp op, const void *args,
                              size_t argc);

/* Replay WAL entries, applying each to structure */
bool persistReplayWAL(persist *p, void *structure);

/* Thread-local mflexState used by per-op applyOp implementations
 * (multilist push/pop need a working mflexState to compress/decompress
 * intermediate flex chunks). persistRecover sets this internally for
 * the duration of the WAL replay; callers that invoke persistOps
 * applyOp OUTSIDE the recover path (e.g. cluster apply mailbox folding
 * delta ops on a follower) must set it before each call and clear
 * after. Pass NULL to clear. Safe to call from any thread; affects
 * only the calling thread. */
struct mflexState;
void persistSetReplayState(struct mflexState *state);
struct mflexState *persistGetReplayState(void);

/* Iterate WAL entries without applying */
typedef bool (*persistWALCallback)(const persistWALEntry *entry, void *ctx);
bool persistIterateWAL(persist *p, persistWALCallback cb, void *ctx);

/* ---- Compaction ---- */

/* Compact WAL into snapshot */
bool persistCompact(persist *p, void *structure);

/* Check if compaction is recommended */
bool persistShouldCompact(const persist *p);

/* ---- Recovery ---- */

/* Full recovery: restore snapshot + replay WAL */
void *persistRecover(persist *p);

/* ---- Lifecycle ---- */

/* Write out buffered WAL entries and sync them; false (counted in
 * persistStats.syncFailures, errno in lastSyncError) when either step
 * fails, leaving the entries buffered or written for the next sync */
bool persistSync(persist *p);

/* Get statistics.  Always writes '*stats' when it is non-NULL: all zeros
 * when 'p' is NULL. */
void persistGetStats(const persist *p, persistStats *stats);

/* Zero every counter (walSequence, the WAL's next sequence, stays) */
void persistResetStats(persist *p);

/* Close and free */
void persistClose(persist *p);

/* ============================================================================
 * Built-in Storage Backends
 * ============================================================================
 */

/* File-based storage */
persistStore *persistStoreFile(const char *path, bool create);

/* Memory-based storage (for testing) */
persistStore *persistStoreMemory(size_t initialCapacity);

/* Get memory store buffer (for inspection/testing) */
const uint8_t *persistStoreMemoryBuffer(persistStore *store, size_t *len);

/* Fault injection: a store wrapping `inner` (it takes ownership; closing it
 * closes `inner`) that passes every call through until an operation kind
 * is armed.  persistStoreFaultyArm(store, op, n) makes that kind's call
 * numbered n (0 = the next) and every later one fail, as a full disk or a
 * dead device would (write/read/seek -1, sync/truncate/rewrite false;
 * errno ENOSPC or EIO).  Disarm restores pass-through.  With `rewrite`,
 * the store offers the atomic rewrite protocol a snapshot publishes with,
 * staging the rewrite in memory and copying it over `inner` on commit, so
 * the crash-safe path runs in tests without files.  Calls counts a kind's
 * calls since it was last armed. */
typedef enum persistFaultOp {
    PERSIST_FAULT_WRITE,
    PERSIST_FAULT_READ,
    PERSIST_FAULT_SEEK,
    PERSIST_FAULT_SYNC,
    PERSIST_FAULT_TRUNCATE,
    PERSIST_FAULT_REWRITE_BEGIN,
    PERSIST_FAULT_REWRITE_COMMIT,
    PERSIST_FAULT_OP_COUNT
} persistFaultOp;
persistStore *persistStoreFaulty(persistStore *inner, bool rewrite);
void persistStoreFaultyArm(persistStore *store, persistFaultOp op,
                           uint64_t nth);
void persistStoreFaultyDisarm(persistStore *store);
uint64_t persistStoreFaultyCalls(const persistStore *store, persistFaultOp op);

/* ============================================================================
 * Built-in persistOps Implementations
 * ============================================================================
 * These are provided for each supported data structure.
 */

extern const persistOps persistOpsFlex;
extern const persistOps persistOpsIntset;
extern const persistOps persistOpsIntsetBig;
extern const persistOps persistOpsMultiroar;
extern const persistOps persistOpsHyperloglog;
extern const persistOps persistOpsMultilist;
extern const persistOps persistOpsMultimap;
/* persistOpsMultidict snapshots record how the dict's keys match:
 * nothing for multidictTypeExactKey (the original layout), a tag for
 * multidictTypeCaseKey and another for any other type.  Its restore
 * rebuilds an exact or case dict from the record (a snapshot without one
 * restores exact, as it always did) and refuses one written by a dict
 * with a custom type, which it cannot rebuild: restoring it under other
 * key matching would merge or split keys.  To persist a dict with a
 * custom type, use a copy of persistOpsMultidict whose .restore calls
 * multidictPersistRestoreAs with that type; it too refuses a snapshot
 * written by a dict of another kind (exact, case or custom). */
extern const persistOps persistOpsMultidict;
struct multidictType;
void *multidictPersistRestoreAs(const uint8_t *data, size_t len,
                                struct multidictType *type);
extern const persistOps persistOpsMultiarray;
extern const persistOps persistOpsMultiOrderedSet;
extern const persistOps persistOpsMultiLRU;

/* Identity-verified POP fold observability (see the apply-arm
 * comment in persist.c): deep-remove = the stamped element was
 * removed from other than the popping end (out-of-order
 * interleave); absent-noop = the stamped element was not in the
 * list at all — the pop's effect had already applied, and the fold
 * deduped instead of blind-popping a different element.  Both are
 * lifetime process totals. */
uint64_t persistMultilistPopIdentityDeepRemoveTotal(void);
uint64_t persistMultilistPopIdentityAbsentNoopTotal(void);

/* ============================================================================
 * Utility Functions
 * ============================================================================
 */

/* Get human-readable name for type */
const char *persistTypeName(persistType type);

/* Get human-readable name for operation */
const char *persistOpName(persistOp op);

/* Verify snapshot file integrity (parses header + unwraps body envelope). */
bool persistVerifySnapshot(persist *p);

/* Verify WAL file integrity (parses header + unwraps every entry envelope). */
bool persistVerifyWAL(persist *p);

#ifdef DATAKIT_TEST
int persistTest(int argc, char *argv[]);
#endif
