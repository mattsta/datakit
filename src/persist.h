#pragma once

/* ============================================================================
 * PERSIST - Pluggable Persistence Framework for Linear Data Structures
 * ============================================================================
 *
 * A clean abstraction for persisting datakit's memory-efficient data structures
 * to disk with support for:
 *   - Full snapshots (point-in-time serialization)
 *   - Write-Ahead Log (incremental operations)
 *   - Compaction (merge WAL into snapshot)
 *   - Crash recovery (snapshot + WAL replay)
 *
 * Architecture Overview:
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │                         Application Layer                               │
 * │   multimap, multilist, multidict, intset, multiOrderedSet, etc.         │
 * └─────────────────────────────────────────────────────────────────────────┘
 *                                    │
 *                     implements persistOps interface
 *                                    ▼
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │                      Persistence Abstraction                            │
 * │                                                                         │
 * │  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐               │
 * │  │  Snapshot    │    │     WAL      │    │  Compaction  │               │
 * │  │  Manager     │    │   Manager    │    │   Engine     │               │
 * │  └──────────────┘    └──────────────┘    └──────────────┘               │
 * │         │                   │                   │                       │
 * │         └───────────────────┴───────────────────┘                       │
 * │                             │                                           │
 * │                    persistStore interface                               │
 * └─────────────────────────────────────────────────────────────────────────┘
 *                                    │
 *                                    ▼
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │                        Storage Backends                                 │
 * │                                                                         │
 * │  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐               │
 * │  │    File      │    │   Memory     │    │   Custom     │               │
 * │  │   Backend    │    │   Backend    │    │   Backend    │               │
 * │  └──────────────┘    └──────────────┘    └──────────────┘               │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * On-Disk Format:
 * ───────────────
 *
 * SNAPSHOT FILE (.snap):
 * ┌────────────────────────────────────────────────────────────────┐
 * │ Header (36 bytes)                                              │
 * │   magic[4]      = "DKSP"                                       │
 * │   version[2]    = format version                               │
 * │   flags[2]      = bit 0: compression                           │
 * │                   bit 1: has_checksum                          │
 * │                   bits 2-3: checksum type (0=none, 1=XXH32,    │
 * │                             2=XXH64, 3=XXH128)                 │
 * │   structType[4] = PERSIST_TYPE_* enum                          │
 * │   count[8]      = element count                                │
 * │   dataLen[8]    = body length                                  │
 * │   checksum[8]   = header checksum (XXH64)                      │
 * ├────────────────────────────────────────────────────────────────┤
 * │ Body (variable)                                                │
 * │   Compressed or raw structure-specific serialized data         │
 * ├────────────────────────────────────────────────────────────────┤
 * │ Footer (variable: 4, 8, or 16 bytes based on checksum type)   │
 * │   bodyChecksum[N] = checksum of body (length depends on type)  │
 * │                     XXH32:  4 bytes                            │
 * │                     XXH64:  8 bytes (default)                  │
 * │                     XXH128: 16 bytes (maximum protection)      │
 * └────────────────────────────────────────────────────────────────┘
 *
 * WAL FILE (.wal):
 * ┌────────────────────────────────────────────────────────────────┐
 * │ WAL Header (24 bytes)                                          │
 * │   magic[4]      = "DKWL"                                       │
 * │   version[2]    = format version                               │
 * │   flags[2]      = options                                      │
 * │   structType[4] = must match snapshot                          │
 * │   sequence[8]   = starting sequence number                     │
 * │   checksum[4]   = header checksum                              │
 * ├────────────────────────────────────────────────────────────────┤
 * │ Entry 0                                                        │
 * │   len[4]        = entry length (excluding len field)           │
 * │   seq[8]        = sequence number                              │
 * │   op[1]         = operation type (PERSIST_OP_*)                │
 * │   data[...]     = operation-specific data                      │
 * │   checksum[4]   = entry checksum                               │
 * ├────────────────────────────────────────────────────────────────┤
 * │ Entry 1...N                                                    │
 * └────────────────────────────────────────────────────────────────┘
 *
 * ============================================================================
 */

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

    /* Bulk operations */
    PERSIST_OP_CLEAR = 20,       /* Remove all elements */
    PERSIST_OP_BULK_INSERT = 21, /* Insert multiple elements */
    PERSIST_OP_BULK_DELETE = 22, /* Delete multiple elements */
    PERSIST_OP_MERGE = 23,       /* Merge another structure */

    /* Structure-specific operations (encoded in data) */
    PERSIST_OP_CUSTOM = 100,

    PERSIST_OP_MAX = 255
} persistOp;

/* ============================================================================
 * Checksum Types
 * ============================================================================
 * The persist framework supports multiple checksum algorithms with different
 * trade-offs between speed, collision resistance, and size.
 *
 * Architecture Design:
 *   - Checksum type is versioned and stored in snapshot/WAL headers
 *   - Variable-length checksums (4/8/16 bytes) for future extensibility
 *   - Type can be changed between snapshots (forward/backward compatible)
 *   - All algorithms use xxHash family for performance and quality
 *
 * Checksum Types:
 *   XXHASH32  - 4 bytes, compact
 *   XXHASH64  - 8 bytes, default (best balance)
 *   XXHASH128 - 16 bytes, maximum collision resistance
 *
 * Chunk-Level Checksums:
 *   For data structures with internal chunks (multimap, multilist), the
 *   persistOps interface supports chunk-level checksumming via:
 *     - getChunkCount()    - returns number of chunks
 *     - snapshotChunk()    - serialize chunk with checksum
 *     - restoreChunk()     - restore chunk with verification
 *
 *   Structures without chunks (intset, flex, multiroar) get a single
 *   whole-structure checksum in the snapshot footer.
 *
 * Checksum Verification:
 *   - Snapshot header: XXH64 (8 bytes)
 *   - Snapshot body: configurable type (default XXH64)
 *   - WAL entries: XXH32 (4 bytes, compact for small entries)
 *   - All checksums verified on restore/recovery
 */
typedef enum persistChecksum {
    PERSIST_CHECKSUM_NONE = 0,      /* No checksum (testing only) */
    PERSIST_CHECKSUM_XXHASH32 = 1,  /* 32-bit xxHash (4 bytes) */
    PERSIST_CHECKSUM_XXHASH64 = 2,  /* 64-bit xxHash (8 bytes, default) */
    PERSIST_CHECKSUM_XXHASH128 = 3, /* 128-bit xxHash (16 bytes) */
} persistChecksum;

/* Maximum checksum size in bytes (for 128-bit hashes) */
#define PERSIST_CHECKSUM_MAX_SIZE 16

/* Checksum value holder supporting variable-length checksums.
 * This structure encapsulates the checksum type and value, allowing
 * different checksum algorithms to coexist in the same codebase and
 * enabling future algorithm additions without breaking compatibility. */
typedef struct persistChecksumValue {
    persistChecksum type; /* Checksum algorithm used */
    uint8_t len;          /* Actual checksum length in bytes (4, 8, or 16) */
    union {
        uint32_t u32;      /* 32-bit checksums (XXH32) */
        uint64_t u64;      /* 64-bit checksums (XXH64) */
        uint8_t bytes[16]; /* Generic byte access (all types) */
        struct {           /* 128-bit checksums (XXH128) */
            uint64_t low64;
            uint64_t high64;
        } u128;
    } value;
} persistChecksumValue;

/* ============================================================================
 * Persistence Operations Interface
 * ============================================================================
 * Each data structure implements this interface to be persistable.
 * The framework is structure-agnostic; all specifics are in these callbacks.
 */
struct persistOps {
    /* Structure identification */
    persistType type;
    const char *name; /* Human-readable name, e.g., "multimap" */

    /* ---- Snapshot Operations ---- */

    /* Serialize entire structure to buffer.
     * Returns allocated buffer (caller frees), sets *len.
     * Returns NULL on error. */
    uint8_t *(*snapshot)(const void *structure, size_t *len);

    /* Deserialize buffer to structure.
     * Returns allocated structure (caller frees).
     * Returns NULL on error. */
    void *(*restore)(const uint8_t *data, size_t len);

    /* Get current element count (for header) */
    size_t (*count)(const void *structure);

    /* Get approximate serialized size (for pre-allocation) */
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
     * structure: mutable pointer to structure
     * op: operation type
     * data: operation data from WAL
     * len: data length
     * Returns true on success. */
    bool (*applyOp)(void *structure, persistOp op, const uint8_t *data,
                    size_t len);

    /* ---- Optional Optimization Hooks ---- */

    /* Stream snapshot incrementally (for very large structures).
     * If NULL, snapshot() is used instead.
     * Callback is called with chunks of data.
     * Returns true on success. */
    bool (*streamSnapshot)(const void *structure,
                           bool (*emit)(const uint8_t *chunk, size_t len,
                                        void *ctx),
                           void *ctx);

    /* Incremental restore (for very large structures).
     * If NULL, restore() is used instead. */
    void *(*streamRestore)(bool (*read)(uint8_t *buf, size_t len, void *ctx),
                           void *ctx);

    /* Validate structure integrity after restore.
     * Returns true if structure is valid. */
    bool (*validate)(const void *structure);

    /* Free structure (if restore allocates) */
    void (*free)(void *structure);

    /* ---- Chunk-Level Checksum Support ---- */

    /* Get number of internal chunks (for chunk-level checksums).
     * Returns 0 if structure doesn't have internal chunks.
     * For structures with chunks (multimap, multilist, etc), returns chunk
     * count. For simple structures (intset, flex, multiroar), returns 0. */
    size_t (*getChunkCount)(const void *structure);

    /* Serialize a specific chunk and return its checksum.
     * chunkIndex: 0-based chunk index
     * Returns allocated buffer with chunk data, sets *len and *checksum.
     * The checksum is computed using the specified checksum type.
     * Returns NULL if chunk doesn't exist or structure doesn't support chunks.
     */
    uint8_t *(*snapshotChunk)(const void *structure, size_t chunkIndex,
                              size_t *len, persistChecksumValue *checksum,
                              persistChecksum checksumType);

    /* Restore a specific chunk with checksum verification.
     * chunkIndex: 0-based chunk index
     * data: chunk data buffer
     * len: chunk data length
     * expectedChecksum: expected checksum value with type
     * Returns true if chunk restored and checksum matches.
     * Returns false on error or checksum mismatch. */
    bool (*restoreChunk)(void *structure, size_t chunkIndex,
                         const uint8_t *data, size_t len,
                         const persistChecksumValue *expectedChecksum);
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

/* Configuration */
persistConfig persistDefaultConfig(void);
void persistSetConfig(persist *p, const persistConfig *config);
void persistGetConfig(const persist *p, persistConfig *config);

/* ---- Snapshot Operations ---- */

/* Take a full snapshot of the structure */
bool persistSnapshot(persist *p, const void *structure);

/* Restore structure from snapshot (allocates new structure) */
void *persistRestore(persist *p);

/* ---- WAL Operations ---- */

/* Log an operation (appends to WAL) */
bool persistLogOp(persist *p, persistOp op, const void *args, size_t argc);

/* Replay WAL entries, applying each to structure */
bool persistReplayWAL(persist *p, void *structure);

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

/* Sync any buffered data */
bool persistSync(persist *p);

/* Get statistics */
void persistGetStats(const persist *p, persistStats *stats);

/* Reset statistics */
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

/* ============================================================================
 * Built-in persistOps Implementations
 * ============================================================================
 * These are provided for each supported data structure.
 */

extern const persistOps persistOpsFlex;
extern const persistOps persistOpsIntset;
extern const persistOps persistOpsMultiroar;
extern const persistOps persistOpsMultilist;
extern const persistOps persistOpsMultimap;
extern const persistOps persistOpsMultidict;
extern const persistOps persistOpsMultiarray;
extern const persistOps persistOpsMultiOrderedSet;
extern const persistOps persistOpsMultiLRU;

/* ============================================================================
 * Utility Functions
 * ============================================================================
 */

/* Get human-readable name for type */
const char *persistTypeName(persistType type);

/* Get human-readable name for operation */
const char *persistOpName(persistOp op);

/* Checksum helpers */
uint32_t persistChecksum32(const void *data, size_t len);
uint64_t persistChecksum64(const void *data, size_t len);

/* Unified checksum computation and verification */
void persistChecksumCompute(persistChecksum type, const void *data, size_t len,
                            persistChecksumValue *out);
bool persistChecksumVerify(const persistChecksumValue *expected,
                           const void *data, size_t len);
bool persistChecksumEqual(const persistChecksumValue *a,
                          const persistChecksumValue *b);

/* Verify snapshot file integrity */
bool persistVerifySnapshot(persistStore *store);

/* Verify WAL file integrity */
bool persistVerifyWAL(persistStore *store);

#ifdef DATAKIT_TEST
int persistTest(int argc, char *argv[]);
#endif
