/* persist.c - Pluggable Persistence Framework for Linear Data Structures
 *
 * See persist.h for API documentation and architecture overview.
 *
 * Implementation follows datakit design principles:
 *   - Self-managing memory (zmalloc/zfree)
 *   - Linear memory-efficient structures
 *   - Clean encapsulation with opaque types
 *   - Comprehensive test coverage
 *
 * SCALABILITY NOTES:
 *
 * Current implementations use non-streaming snapshot/restore which buffers
 * the entire serialized structure in memory. This is efficient for structures
 * up to several GB. For terabyte-scale data (e.g., multilistFull with billions
 * of entries), use the streaming API:
 *
 *   1. streamSnapshot - Writes data in chunks via callback, never buffering
 *      the entire structure. For multilist: iterate nodes, serialize each
 *      node independently, call callback per node.
 *
 *   2. streamRestore - Reads data in chunks via callback, building structure
 *      incrementally. For multilist: receive node data via callback, append
 *      each node to growing structure.
 *
 * The streaming callbacks are defined in persistOps but implementations are
 * marked NULL for the current non-streaming approach. When streaming is needed,
 * implement streamSnapshot/streamRestore for the relevant data type.
 *
 * For extremely large structures, also consider:
 *   - Incremental snapshots (delta encoding between snapshots)
 *   - Parallel restore (multi-threaded node reconstruction)
 *   - Memory-mapped I/O for the storage backend
 */

#include "persist.h"
#include "databoxLinear.h"
#include "datakit.h"
#include "mflex.h"

#define XXH_STATIC_LINKING_ONLY
#include "../deps/xxHash/xxhash.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef DATAKIT_TEST
#include "perf.h"
#endif

/* ============================================================================
 * Magic Numbers and Format Constants
 * ============================================================================
 */
#define PERSIST_SNAP_MAGIC 0x50534B44 /* "DKSP" little-endian */
#define PERSIST_WAL_MAGIC 0x4C574B44  /* "DKWL" little-endian */
#define PERSIST_VERSION 1

#define PERSIST_SNAP_HEADER_SIZE 36
#define PERSIST_WAL_HEADER_SIZE 24

/* ============================================================================
 * Checksum Implementation
 * ============================================================================
 * Unified checksum interface supporting multiple hash algorithms.
 * Uses xxHash for high performance and excellent distribution.
 *
 * Checksum Type Support:
 *   - PERSIST_CHECKSUM_NONE:      No checksum (returns 0)
 *   - PERSIST_CHECKSUM_XXHASH32:  XXH32 (32-bit, legacy header use)
 *   - PERSIST_CHECKSUM_XXHASH64:  XXH64 (64-bit, default for body/chunks)
 *   - PERSIST_CHECKSUM_XXHASH128: XXH128 (128-bit, future-proof)
 *   - PERSIST_CHECKSUM_CRC32C:    CRC32C (not yet implemented)
 *
 * For 128-bit checksums, we fold the hash down to 64 bits for storage
 * compatibility by XORing the high and low halves.
 */

/* Compute checksum using specified algorithm */
void persistChecksumCompute(persistChecksum type, const void *data, size_t len,
                            persistChecksumValue *out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->type = type;

    if (!data || len == 0) {
        out->len = 0;
        return;
    }

    switch (type) {
    case PERSIST_CHECKSUM_NONE:
        out->len = 0;
        break;

    case PERSIST_CHECKSUM_XXHASH32:
        out->len = 4;
        out->value.u32 = XXH32(data, len, 0);
        break;

    case PERSIST_CHECKSUM_XXHASH64:
        out->len = 8;
        out->value.u64 = XXH64(data, len, 0);
        break;

    case PERSIST_CHECKSUM_XXHASH128: {
        out->len = 16;
        XXH128_hash_t hash128 = XXH128(data, len, 0);
        out->value.u128.low64 = hash128.low64;
        out->value.u128.high64 = hash128.high64;
        break;
    }

    default:
        out->len = 0;
        break;
    }
}

/* Compare two checksum values for equality */
bool persistChecksumEqual(const persistChecksumValue *a,
                          const persistChecksumValue *b) {
    if (!a || !b) {
        return false;
    }

    if (a->type != b->type || a->len != b->len) {
        return false;
    }

    /* Compare based on length */
    return memcmp(a->value.bytes, b->value.bytes, a->len) == 0;
}

/* Verify checksum matches expected value */
bool persistChecksumVerify(const persistChecksumValue *expected,
                           const void *data, size_t len) {
    if (!expected || !data) {
        return false;
    }

    persistChecksumValue computed;
    persistChecksumCompute(expected->type, data, len, &computed);
    return persistChecksumEqual(expected, &computed);
}

/* Legacy wrappers for existing code (map to XXH32/XXH64) */
uint32_t persistChecksum32(const void *data, size_t len) {
    return (uint32_t)XXH32(data, len, 0);
}

uint64_t persistChecksum64(const void *data, size_t len) {
    return XXH64(data, len, 0);
}

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

    /* WAL state */
    uint64_t walSequence;  /* Next sequence number */
    uint8_t *walBuffer;    /* Write buffer */
    size_t walBufferUsed;  /* Bytes used in buffer */
    uint64_t lastSyncTime; /* Last sync timestamp (microseconds) */
    bool walInitialized;   /* Whether WAL header has been written */
};

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
        .checksumType = PERSIST_CHECKSUM_XXHASH32,
        .syncMode = PERSIST_SYNC_EVERYSEC,
        .walMaxSize = 64 * 1024 * 1024, /* 64 MB */
        .walBufferSize = 64 * 1024,     /* 64 KB */
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

    persist *p = zmalloc(sizeof(*p));
    p->ops = ops;
    p->config = config ? *config : persistDefaultConfig();
    p->snapshotStore = NULL;
    p->walStore = NULL;
    memset(&p->stats, 0, sizeof(p->stats));

    p->walSequence = 1;
    p->walBuffer = NULL;
    p->walBufferUsed = 0;
    p->lastSyncTime = persistGetMicroseconds();
    p->walInitialized = false;

    /* Allocate WAL buffer if needed */
    if (p->config.walBufferSize > 0) {
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

void persistSetConfig(persist *p, const persistConfig *config) {
    if (p && config) {
        /* Check if buffer size is changing before updating config */
        size_t oldBufferSize = p->config.walBufferSize;
        p->config = *config;

        /* Reallocate WAL buffer if size changed */
        if (oldBufferSize != config->walBufferSize) {
            zfree(p->walBuffer);
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
    if (p && stats) {
        *stats = p->stats;
        stats->walSequence = p->walSequence;
    }
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

/* Snapshot header format (32 bytes):
 *   0-3:   magic (4 bytes)
 *   4-5:   version (2 bytes)
 *   6-7:   flags (2 bytes)
 *   8-11:  structType (4 bytes)
 *   12-19: count (8 bytes)
 *   20-27: dataLen (8 bytes)
 *   28-35: headerChecksum (8 bytes)
 */
typedef struct __attribute__((packed)) persistSnapHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t structType;
    uint64_t count;
    uint64_t dataLen;
    uint64_t headerChecksum;
} persistSnapHeader;

DK_SIZECHECK(persistSnapHeader, PERSIST_SNAP_HEADER_SIZE);

/* Flags bit definitions */
#define PERSIST_FLAG_HAS_CHECKSUM (1 << 1)

bool persistSnapshot(persist *p, const void *structure) {
    if (!p || !structure || !p->snapshotStore || !p->ops->snapshot) {
        return false;
    }

    uint64_t startTime = persistGetMicroseconds();

    /* Serialize the structure */
    size_t dataLen = 0;
    uint8_t *data = p->ops->snapshot(structure, &dataLen);
    if (!data) {
        return false;
    }

    /* Build header */
    persistSnapHeader header = {
        .magic = PERSIST_SNAP_MAGIC,
        .version = PERSIST_VERSION,
        .flags =
            PERSIST_FLAG_HAS_CHECKSUM | ((p->config.checksumType & 0x3) << 2),
        .structType = p->ops->type,
        .count = p->ops->count ? p->ops->count(structure) : 0,
        .dataLen = dataLen,
        .headerChecksum = 0,
    };

    /* Compute header checksum (excluding the checksum field itself) */
    header.headerChecksum = persistChecksum64(&header, 28);

    /* Seek to beginning */
    persistStore *store = p->snapshotStore;
    if (store->seek(store->ctx, 0, SEEK_SET) < 0) {
        zfree(data);
        return false;
    }

    /* Write header */
    if (store->write(store->ctx, &header, sizeof(header)) != sizeof(header)) {
        zfree(data);
        return false;
    }

    /* Write body */
    if (store->write(store->ctx, data, dataLen) != (ssize_t)dataLen) {
        zfree(data);
        return false;
    }

    /* Compute and write body checksum using configured type */
    persistChecksumValue bodyChecksum;
    persistChecksumCompute(p->config.checksumType, data, dataLen,
                           &bodyChecksum);

    /* Write checksum bytes (variable length based on type) */
    if (bodyChecksum.len > 0) {
        if (store->write(store->ctx, bodyChecksum.value.bytes,
                         bodyChecksum.len) != (ssize_t)bodyChecksum.len) {
            zfree(data);
            return false;
        }
    }

    /* Truncate any trailing data from previous snapshot */
    if (store->truncate) {
        store->truncate(store->ctx);
    }

    /* Sync to disk */
    if (store->sync) {
        store->sync(store->ctx);
    }

    zfree(data);

    /* Update stats */
    p->stats.snapshotCount++;
    p->stats.snapshotBytes += sizeof(header) + dataLen + bodyChecksum.len;
    p->stats.lastSnapshotTime = startTime;
    p->stats.lastSnapshotDuration = persistGetMicroseconds() - startTime;

    return true;
}

void *persistRestore(persist *p) {
    if (!p || !p->snapshotStore || !p->ops->restore) {
        return NULL;
    }

    persistStore *store = p->snapshotStore;

    /* Seek to beginning */
    if (store->seek(store->ctx, 0, SEEK_SET) < 0) {
        return NULL;
    }

    /* Read header */
    persistSnapHeader header;
    if (store->read(store->ctx, &header, sizeof(header)) != sizeof(header)) {
        return NULL;
    }

    /* Verify magic */
    if (header.magic != PERSIST_SNAP_MAGIC) {
        return NULL;
    }

    /* Verify version */
    if (header.version > PERSIST_VERSION) {
        return NULL;
    }

    /* Verify header checksum */
    uint64_t savedChecksum = header.headerChecksum;
    header.headerChecksum = 0;
    uint64_t computedChecksum = persistChecksum64(&header, 28);
    if (savedChecksum != computedChecksum) {
        return NULL;
    }
    header.headerChecksum = savedChecksum;

    /* Verify structure type matches */
    if (header.structType != p->ops->type) {
        return NULL;
    }

    /* Read body */
    uint8_t *data = zmalloc(header.dataLen);
    if (store->read(store->ctx, data, header.dataLen) !=
        (ssize_t)header.dataLen) {
        zfree(data);
        return NULL;
    }

    /* Verify body checksum if present */
    if (header.flags & PERSIST_FLAG_HAS_CHECKSUM) {
        /* Extract checksum type from flags (bits 2-3) */
        persistChecksum checksumType = (header.flags >> 2) & 0x3;

        /* Compute expected checksum */
        persistChecksumValue expectedChecksum;
        persistChecksumCompute(checksumType, data, header.dataLen,
                               &expectedChecksum);

        /* Read stored checksum bytes */
        uint8_t storedBytes[PERSIST_CHECKSUM_MAX_SIZE];
        if (expectedChecksum.len > 0) {
            if (store->read(store->ctx, storedBytes, expectedChecksum.len) !=
                (ssize_t)expectedChecksum.len) {
                zfree(data);
                return NULL;
            }

            /* Verify checksum matches */
            if (memcmp(storedBytes, expectedChecksum.value.bytes,
                       expectedChecksum.len) != 0) {
                zfree(data);
                return NULL;
            }
        }
    }

    /* Restore structure */
    void *structure = p->ops->restore(data, header.dataLen);
    zfree(data);

    /* Validate if validator is available */
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

/* WAL header format (24 bytes):
 *   0-3:   magic (4 bytes)
 *   4-5:   version (2 bytes)
 *   6-7:   flags (2 bytes)
 *   8-11:  structType (4 bytes)
 *   12-19: startSequence (8 bytes)
 *   20-23: headerChecksum (4 bytes)
 */
typedef struct __attribute__((packed)) persistWALHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t structType;
    uint64_t startSequence;
    uint32_t headerChecksum;
} persistWALHeader;

DK_SIZECHECK(persistWALHeader, PERSIST_WAL_HEADER_SIZE);

/* Initialize WAL file with header */
static bool persistWALInit(persist *p) {
    if (!p->walStore) {
        return false;
    }

    persistStore *store = p->walStore;

    /* Seek to beginning */
    if (store->seek(store->ctx, 0, SEEK_SET) < 0) {
        return false;
    }

    /* Check if WAL already has a valid header */
    persistWALHeader header;
    if (store->read(store->ctx, &header, sizeof(header)) == sizeof(header)) {
        if (header.magic == PERSIST_WAL_MAGIC) {
            /* WAL exists, seek to end for appending */
            store->seek(store->ctx, 0, SEEK_END);
            return true;
        }
    }

    /* Write new header */
    header = (persistWALHeader){
        .magic = PERSIST_WAL_MAGIC,
        .version = PERSIST_VERSION,
        .flags = 0,
        .structType = p->ops->type,
        .startSequence = p->walSequence,
        .headerChecksum = 0,
    };
    header.headerChecksum = persistChecksum32(&header, 20);

    if (store->seek(store->ctx, 0, SEEK_SET) < 0) {
        return false;
    }

    if (store->write(store->ctx, &header, sizeof(header)) != sizeof(header)) {
        return false;
    }

    if (store->sync) {
        store->sync(store->ctx);
    }

    return true;
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

bool persistLogOp(persist *p, persistOp op, const void *args, size_t argc) {
    if (!p || !p->walStore || !p->ops->encodeOp) {
        return false;
    }

    /* Initialize WAL if needed */
    if (!p->walInitialized) {
        if (!persistWALInit(p)) {
            return false;
        }
        p->walInitialized = true;
    }

    /* Encode the operation */
    size_t dataLen = 0;
    uint8_t *data = p->ops->encodeOp(op, args, argc, &dataLen);
    if (!data && dataLen > 0) {
        return false;
    }

    /* WAL entry format:
     *   length (4 bytes) - total entry length excluding this field
     *   sequence (8 bytes)
     *   op (1 byte)
     *   data (variable)
     *   checksum (4 bytes)
     */
    uint32_t entryLen = 8 + 1 + dataLen + 4; /* seq + op + data + checksum */

    /* Build entry header */
    uint8_t entryHeader[13]; /* length(4) + seq(8) + op(1) */
    memcpy(entryHeader, &entryLen, 4);
    memcpy(entryHeader + 4, &p->walSequence, 8);
    entryHeader[12] = (uint8_t)op;

    /* Compute checksum over seq + op + data */
    XXH32_state_t *state = XXH32_createState();
    XXH32_reset(state, 0);
    XXH32_update(state, entryHeader + 4, 9);
    if (data && dataLen > 0) {
        XXH32_update(state, data, dataLen);
    }
    uint32_t checksum = XXH32_digest(state);
    XXH32_freeState(state);

    /* Write entry */
    if (!persistWALWrite(p, entryHeader, sizeof(entryHeader))) {
        zfree(data);
        return false;
    }

    if (data && dataLen > 0) {
        if (!persistWALWrite(p, data, dataLen)) {
            zfree(data);
            return false;
        }
    }

    if (!persistWALWrite(p, &checksum, sizeof(checksum))) {
        zfree(data);
        return false;
    }

    zfree(data);

    /* Update state */
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

bool persistReplayWAL(persist *p, void *structure) {
    if (!p || !structure || !p->walStore || !p->ops->applyOp) {
        return false;
    }

    persistStore *store = p->walStore;

    /* Seek past header */
    if (store->seek(store->ctx, PERSIST_WAL_HEADER_SIZE, SEEK_SET) < 0) {
        return false;
    }

    /* Read and apply entries */
    while (1) {
        /* Read entry length */
        uint32_t entryLen;
        ssize_t n = store->read(store->ctx, &entryLen, 4);
        if (n == 0) {
            break; /* End of WAL */
        }
        if (n != 4) {
            if (p->config.strictRecovery) {
                return false;
            }
            break; /* Truncated entry */
        }

        /* Sanity check entry length */
        if (entryLen < 13 || entryLen > 100 * 1024 * 1024) { /* Max 100MB */
            if (p->config.strictRecovery) {
                return false;
            }
            break;
        }

        /* Read entry body */
        uint8_t *entry = zmalloc(entryLen);
        if (store->read(store->ctx, entry, entryLen) != (ssize_t)entryLen) {
            zfree(entry);
            if (p->config.strictRecovery) {
                return false;
            }
            break;
        }

        /* Parse entry */
        uint64_t seq;
        memcpy(&seq, entry, 8);
        persistOp op = entry[8];
        size_t dataLen = entryLen - 13;
        const uint8_t *data = entry + 9;
        uint32_t storedChecksum;
        memcpy(&storedChecksum, entry + entryLen - 4, 4);

        /* Verify checksum */
        uint32_t computedChecksum = persistChecksum32(entry, entryLen - 4);
        if (storedChecksum != computedChecksum) {
            zfree(entry);
            p->stats.entriesSkipped++;
            if (p->config.strictRecovery) {
                return false;
            }
            continue; /* Skip bad entry in lenient mode */
        }

        /* Apply operation */
        if (!p->ops->applyOp(structure, op, data, dataLen)) {
            zfree(entry);
            p->stats.entriesSkipped++;
            if (p->config.strictRecovery) {
                return false;
            }
            continue;
        }

        /* Update sequence to be after this entry */
        if (seq >= p->walSequence) {
            p->walSequence = seq + 1;
        }

        p->stats.entriesRecovered++;
        zfree(entry);
    }

    return true;
}

bool persistIterateWAL(persist *p, persistWALCallback cb, void *ctx) {
    if (!p || !p->walStore || !cb) {
        return false;
    }

    persistStore *store = p->walStore;

    /* Seek past header */
    if (store->seek(store->ctx, PERSIST_WAL_HEADER_SIZE, SEEK_SET) < 0) {
        return false;
    }

    /* Read entries */
    while (1) {
        uint32_t entryLen;
        if (store->read(store->ctx, &entryLen, 4) != 4) {
            break;
        }

        if (entryLen < 13 || entryLen > 100 * 1024 * 1024) {
            break;
        }

        uint8_t *entry = zmalloc(entryLen);
        if (store->read(store->ctx, entry, entryLen) != (ssize_t)entryLen) {
            zfree(entry);
            break;
        }

        /* Parse and invoke callback */
        persistWALEntry walEntry = {
            .sequence = 0,
            .op = entry[8],
            .data = entry + 9,
            .len = entryLen - 13,
        };
        memcpy(&walEntry.sequence, entry, 8);

        bool cont = cb(&walEntry, ctx);
        zfree(entry);

        if (!cont) {
            break;
        }
    }

    return true;
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

    /* Take new snapshot */
    if (!persistSnapshot(p, structure)) {
        return false;
    }

    /* Truncate WAL */
    if (p->walStore) {
        persistStore *store = p->walStore;
        if (store->seek(store->ctx, 0, SEEK_SET) >= 0) {
            if (store->truncate) {
                store->truncate(store->ctx);
            }
        }

        /* Clear any buffered WAL entries - they're now in the snapshot */
        p->walBufferUsed = 0;

        /* Reinitialize WAL with new header */
        persistWALHeader header = {
            .magic = PERSIST_WAL_MAGIC,
            .version = PERSIST_VERSION,
            .flags = 0,
            .structType = p->ops->type,
            .startSequence = p->walSequence,
            .headerChecksum = 0,
        };
        header.headerChecksum = persistChecksum32(&header, 20);

        store->seek(store->ctx, 0, SEEK_SET);
        store->write(store->ctx, &header, sizeof(header));
        if (store->sync) {
            store->sync(store->ctx);
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

    /* Create mflexState for this recovery session if structure uses multilist
     */
    if (p->ops->type == PERSIST_TYPE_MULTILIST) {
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

bool persistVerifySnapshot(persistStore *store) {
    if (!store) {
        return false;
    }

    /* Seek to beginning */
    if (store->seek(store->ctx, 0, SEEK_SET) < 0) {
        return false;
    }

    /* Read and verify header */
    persistSnapHeader header;
    if (store->read(store->ctx, &header, sizeof(header)) != sizeof(header)) {
        return false;
    }

    if (header.magic != PERSIST_SNAP_MAGIC) {
        return false;
    }

    if (header.version > PERSIST_VERSION) {
        return false;
    }

    uint64_t savedChecksum = header.headerChecksum;
    header.headerChecksum = 0;
    if (persistChecksum64(&header, 28) != savedChecksum) {
        return false;
    }

    /* Read body and verify checksum */
    if (header.flags & PERSIST_FLAG_HAS_CHECKSUM) {
        uint8_t *data = zmalloc(header.dataLen);
        if (store->read(store->ctx, data, header.dataLen) !=
            (ssize_t)header.dataLen) {
            zfree(data);
            return false;
        }

        /* Extract checksum type from flags (bits 2-3) */
        persistChecksum checksumType = (header.flags >> 2) & 0x3;

        /* Compute expected checksum */
        persistChecksumValue expectedChecksum;
        persistChecksumCompute(checksumType, data, header.dataLen,
                               &expectedChecksum);

        /* Read stored checksum bytes */
        uint8_t storedBytes[PERSIST_CHECKSUM_MAX_SIZE];
        if (expectedChecksum.len > 0) {
            if (store->read(store->ctx, storedBytes, expectedChecksum.len) !=
                (ssize_t)expectedChecksum.len) {
                zfree(data);
                return false;
            }

            /* Verify checksum matches */
            if (memcmp(storedBytes, expectedChecksum.value.bytes,
                       expectedChecksum.len) != 0) {
                zfree(data);
                return false;
            }
        }

        zfree(data);
    }

    return true;
}

bool persistVerifyWAL(persistStore *store) {
    if (!store) {
        return false;
    }

    /* Seek to beginning */
    if (store->seek(store->ctx, 0, SEEK_SET) < 0) {
        return false;
    }

    /* Read and verify header */
    persistWALHeader header;
    if (store->read(store->ctx, &header, sizeof(header)) != sizeof(header)) {
        return false;
    }

    if (header.magic != PERSIST_WAL_MAGIC) {
        return false;
    }

    if (header.version > PERSIST_VERSION) {
        return false;
    }

    uint32_t savedChecksum = header.headerChecksum;
    header.headerChecksum = 0;
    if (persistChecksum32(&header, 20) != savedChecksum) {
        return false;
    }

    /* Verify all entries */
    while (1) {
        uint32_t entryLen;
        if (store->read(store->ctx, &entryLen, 4) != 4) {
            break; /* End of file */
        }

        if (entryLen < 13 || entryLen > 100 * 1024 * 1024) {
            return false;
        }

        uint8_t *entry = zmalloc(entryLen);
        if (store->read(store->ctx, entry, entryLen) != (ssize_t)entryLen) {
            zfree(entry);
            return false; /* Truncated entry */
        }

        uint32_t storedChecksum;
        memcpy(&storedChecksum, entry + entryLen - 4, 4);
        uint32_t computed = persistChecksum32(entry, entryLen - 4);
        zfree(entry);

        if (computed != storedChecksum) {
            return false;
        }
    }

    return true;
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
    persistMemStore *m = zmalloc(sizeof(*m));
    m->data = initialCapacity > 0 ? zmalloc(initialCapacity) : NULL;
    m->size = 0;
    m->capacity = initialCapacity;
    m->position = 0;

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
    int fd;
    char *path;
} persistFileStore;

static ssize_t persistFileWrite(void *ctx, const void *data, size_t len) {
    persistFileStore *f = ctx;
    return write(f->fd, data, len);
}

static ssize_t persistFileRead(void *ctx, void *buf, size_t len) {
    persistFileStore *f = ctx;
    return read(f->fd, buf, len);
}

static int64_t persistFileSeek(void *ctx, int64_t offset, int whence) {
    persistFileStore *f = ctx;
    return lseek(f->fd, offset, whence);
}

static int64_t persistFileTell(void *ctx) {
    persistFileStore *f = ctx;
    return lseek(f->fd, 0, SEEK_CUR);
}

static bool persistFileSync(void *ctx) {
    persistFileStore *f = ctx;
    return dk_fsync(f->fd) == 0;
}

static bool persistFileTruncate(void *ctx) {
    persistFileStore *f = ctx;
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) {
        return false;
    }
    return ftruncate(f->fd, pos) == 0;
}

static int64_t persistFileSize(void *ctx) {
    persistFileStore *f = ctx;
    struct stat st;
    if (fstat(f->fd, &st) < 0) {
        return -1;
    }
    return st.st_size;
}

static void persistFileClose(void *ctx) {
    persistFileStore *f = ctx;
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

    persistFileStore *f = zmalloc(sizeof(*f));
    f->fd = fd;
    f->path = zmalloc(strlen(path) + 1);
    strcpy(f->path, path);

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
    uint8_t *buf = zmalloc(*len);
    memcpy(buf, f, *len);
    return buf;
}

static void *flexPersistRestore(const uint8_t *data, size_t len) {
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
    /* args is expected to be const databox* for most operations */
    const databox *box = args;

    switch (op) {
    case PERSIST_OP_PUSH_HEAD:
    case PERSIST_OP_PUSH_TAIL:
    case PERSIST_OP_INSERT:
    case PERSIST_OP_DELETE: {
        if (!box || argc < 1) {
            *len = 0;
            return NULL;
        }

        /* Encode single databox using databoxLinear */
        databoxLinear dl;
        size_t encodedLength;
        const void *encodedValue;
        uint8_t encodedType;
        DATABOX_LINEAR_PARTS_ENCODE(box, encodedLength, encodedValue,
                                    encodedType, &dl);

        *len = 1 + encodedLength; /* type + value */
        uint8_t *buf = zmalloc(*len);
        buf[0] = encodedType;
        memcpy(buf + 1, encodedValue, encodedLength);
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

static bool flexPersistApplyOp(void *structure, persistOp op,
                               const uint8_t *data, size_t len) {
    flex **ff = structure; /* We expect a pointer to the flex pointer */

    switch (op) {
    case PERSIST_OP_PUSH_HEAD:
    case PERSIST_OP_PUSH_TAIL: {
        if (len < 1) {
            return false;
        }

        databox box;
        DATABOX_LINEAR_PARTS_DECODE(data[0], data + 1, len - 1, &box);

        flexPushByType(ff, &box,
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

const persistOps persistOpsFlex = {
    .type = PERSIST_TYPE_FLEX,
    .name = "flex",
    .snapshot = flexPersistSnapshot,
    .restore = flexPersistRestore,
    .count = flexPersistCount,
    .estimateSize = flexPersistEstimateSize,
    .encodeOp = flexPersistEncodeOp,
    .applyOp = flexPersistApplyOp,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = NULL,
    .free = flexPersistFree,
};

/* ============================================================================
 * Structure-Specific Operations: intset
 * ============================================================================
 */
#include "intset.h"

static uint8_t *intsetPersistSnapshot(const void *structure, size_t *len) {
    intset *is = (intset *)structure;
    size_t count = intsetCount(is);

    /* Snapshot format: [count: 8 bytes][values: count * 8 bytes]
     * We serialize all values as int64_t for universality */
    *len = sizeof(uint64_t) + (count * sizeof(int64_t));
    uint8_t *buf = zmalloc(*len);

    /* Write count */
    uint64_t countU64 = count;
    memcpy(buf, &countU64, sizeof(uint64_t));

    /* Write all values using intsetGet */
    int64_t *values = (int64_t *)(buf + sizeof(uint64_t));
    for (size_t i = 0; i < count; i++) {
        int64_t value;
        if (intsetGet(is, (uint32_t)i, &value)) {
            values[i] = value;
        } else {
            values[i] = 0; /* Should not happen */
        }
    }

    return buf;
}

static void *intsetPersistRestore(const uint8_t *data, size_t len) {
    /* Minimum valid length: count field */
    if (len < sizeof(uint64_t)) {
        return NULL;
    }

    /* Read count */
    uint64_t count;
    memcpy(&count, data, sizeof(uint64_t));

    /* Validate length */
    size_t expectedLen = sizeof(uint64_t) + (count * sizeof(int64_t));
    if (len < expectedLen) {
        return NULL;
    }

    /* Create new intset and add all values */
    intset *is = intsetNew();
    const int64_t *values = (const int64_t *)(data + sizeof(uint64_t));

    for (uint64_t i = 0; i < count; i++) {
        bool success;
        intsetAdd(&is, values[i], &success);
    }

    return is;
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

const persistOps persistOpsIntset = {
    .type = PERSIST_TYPE_INTSET,
    .name = "intset",
    .snapshot = intsetPersistSnapshot,
    .restore = intsetPersistRestore,
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
 * Structure-Specific Operations: multiroar
 * ============================================================================
 */
#include "multiroar.h"

static uint8_t *multiroarPersistSnapshot(const void *structure, size_t *len) {
    const multiroar *r = structure;

    /* Get serialized size first */
    size_t size = multiroarSerializedSize(r);
    uint8_t *buf = zmalloc(size);

    /* Serialize into buffer */
    size_t written = multiroarSerialize(r, buf, size);
    if (written == 0 || written != size) {
        zfree(buf);
        *len = 0;
        return NULL;
    }

    *len = written;
    return buf;
}

static void *multiroarPersistRestore(const uint8_t *data, size_t len) {
    /* Deserialize multiroar from snapshot */
    return multiroarDeserialize(data, len);
}

static size_t multiroarPersistCount(const void *structure) {
    return multiroarBitCount(structure);
}

static size_t multiroarPersistEstimateSize(const void *structure) {
    return multiroarMemoryUsage(structure);
}

static uint8_t *multiroarPersistEncodeOp(persistOp op, const void *args,
                                         size_t argc, size_t *len) {
    (void)argc;

    switch (op) {
    case PERSIST_OP_INSERT:
    case PERSIST_OP_DELETE: {
        /* args is uint64_t* (position) */
        const uint64_t *position = args;
        *len = sizeof(uint64_t);
        uint8_t *buf = zmalloc(*len);
        memcpy(buf, position, sizeof(uint64_t));
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

    case PERSIST_OP_CLEAR: {
        multiroarFree(*r);
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

const persistOps persistOpsMultiroar = {
    .type = PERSIST_TYPE_MULTIROAR,
    .name = "multiroar",
    .snapshot = multiroarPersistSnapshot,
    .restore = multiroarPersistRestore,
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
 * Structure-Specific Operations: multimap
 * ============================================================================
 */
#include "multimap.h"

/* Get elementsPerEntry using the iterator interface (avoids internal headers)
 */
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

    /* Snapshot format: [elementsPerEntry: 4 bytes][flex data: N bytes] */
    size_t flexLen = flexBytes(f);
    *len = sizeof(uint32_t) + flexLen;
    uint8_t *buf = zmalloc(*len);

    memcpy(buf, &elementsPerEntry, sizeof(uint32_t));
    memcpy(buf + sizeof(uint32_t), f, flexLen);

    flexFree(f);
    return buf;
}

static void *multimapPersistRestore(const uint8_t *data, size_t len) {
    /* Snapshot format: [elementsPerEntry: 4 bytes][flex data: N bytes] */
    if (len < sizeof(uint32_t)) {
        return NULL;
    }

    /* Read elementsPerEntry */
    uint32_t elementsPerEntry;
    memcpy(&elementsPerEntry, data, sizeof(uint32_t));
    if (elementsPerEntry == 0 || elementsPerEntry > 1024) {
        return NULL; /* Sanity check */
    }

    /* Restore the flex from remaining data */
    size_t flexLen = len - sizeof(uint32_t);
    flex *f = zmalloc(flexLen);
    memcpy(f, data + sizeof(uint32_t), flexLen);

    /* Create multimap */
    multimap *m = multimapNew(elementsPerEntry);

    /* Iterate through entries in the flex
     * Note: flexHead returns non-NULL even for empty flex,
     * so we must check count */
    size_t count = flexCount(f);
    size_t numEntries = count / elementsPerEntry;
    flexEntry *fe = flexHead(f);

    databox *elements[elementsPerEntry];
    databox elementStorage[elementsPerEntry];
    for (uint32_t i = 0; i < elementsPerEntry; i++) {
        elements[i] = &elementStorage[i];
    }

    for (size_t e = 0; e < numEntries; e++) {
        /* Read elementsPerEntry databoxes for one entry */
        for (uint32_t i = 0; i < elementsPerEntry; i++) {
            flexGetByType(fe, elements[i]);
            fe = flexNext(f, fe);
        }
        multimapInsertFullWidth(&m, (const databox **)elements);
    }

    flexFree(f);
    return m;
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
        /* Encode: [argc as varint][box0][box1]... */
        /* First pass: calculate total length */
        size_t totalLen = 1; /* 1 byte for argc (simple encoding) */
        databoxLinear dls[argc];

        for (size_t i = 0; i < argc; i++) {
            size_t encodedLen;
            const void *encodedVal;
            uint8_t encodedType;
            DATABOX_LINEAR_PARTS_ENCODE(boxes[i], encodedLen, encodedVal,
                                        encodedType, &dls[i]);
            (void)encodedVal;  /* Set by macro but only encodedLen is needed */
            (void)encodedType; /* Set by macro but only encodedLen is needed */
            totalLen += 1 + encodedLen; /* type + value */
        }

        *len = totalLen;
        uint8_t *buf = zmalloc(totalLen);
        buf[0] = (uint8_t)argc;

        size_t offset = 1;
        for (size_t i = 0; i < argc; i++) {
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
        /* Just encode the key (first element) */
        if (argc < 1 || !boxes[0]) {
            *len = 0;
            return NULL;
        }

        databoxLinear dl;
        size_t encodedLen;
        const void *encodedVal;
        uint8_t encodedType;
        DATABOX_LINEAR_PARTS_ENCODE(boxes[0], encodedLen, encodedVal,
                                    encodedType, &dl);

        *len = 1 + encodedLen;
        uint8_t *buf = zmalloc(*len);
        buf[0] = encodedType;
        memcpy(buf + 1, encodedVal, encodedLen);
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

/* Helper to determine encoded value length from databoxLinear type byte */
static size_t databoxLinearTypeValueLen(uint8_t type) {
    /* Type byte encoding (from databoxLinear.c):
     *   0: invalid
     *   1: BYTES (variable, handled separately)
     *   2-17: integers (2-3 = 1 byte, 4-5 = 2 bytes, ... 16-17 = 8 bytes)
     *   18: REAL_16B (2 bytes)
     *   19: REAL_32B (4 bytes)
     *   20: REAL_64B (8 bytes)
     *   21-23: TRUE/FALSE/NULL (0 bytes)
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

    switch (op) {
    case PERSIST_OP_INSERT: {
        if (len < 2) {
            return false;
        }

        uint8_t argc = data[0];
        if (argc == 0 || argc > 32) {
            return false;
        }

        databox boxes[argc];
        const databox *boxPtrs[argc];
        for (size_t i = 0; i < argc; i++) {
            boxPtrs[i] = &boxes[i];
        }

        size_t offset = 1;
        for (size_t i = 0; i < argc && offset < len; i++) {
            uint8_t type = data[offset++];

            /* Determine value length based on type */
            size_t valueLen;
            if (DATABOX_LINEAR_TYPE_IS_BYTES(type)) {
                /* For bytes, remaining data is the value */
                valueLen = len - offset;
                if (i < argc - 1) {
                    /* This is tricky - we need length prefix for BYTES in
                     * multi-value */
                    /* For now, assume single value or last value is BYTES */
                    return false;
                }
            } else {
                /* Fixed-width type - calculate actual length from type */
                valueLen = databoxLinearTypeValueLen(type);
                if (offset + valueLen > len) {
                    return false; /* Not enough data */
                }
            }

            DATABOX_LINEAR_PARTS_DECODE(type, data + offset, valueLen,
                                        &boxes[i]);
            offset += valueLen;
        }

        multimapInsert(m, boxPtrs);
        return true;
    }

    case PERSIST_OP_DELETE: {
        if (len < 1) {
            return false;
        }

        databox key;
        DATABOX_LINEAR_PARTS_DECODE(data[0], data + 1, len - 1, &key);
        multimapDelete(m, &key);
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

const persistOps persistOpsMultimap = {
    .type = PERSIST_TYPE_MULTIMAP,
    .name = "multimap",
    .snapshot = multimapPersistSnapshot,
    .restore = multimapPersistRestore,
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

static uint8_t *multilistPersistSnapshot(const void *structure, size_t *len) {
    const multilist *ml = structure;

    /* Create a flex to hold all elements */
    flex *f = flexNew();

    /* Iterate through all elements and add to flex */
    mflexState *state[2] = {mflexStateCreate(), mflexStateCreate()};
    multilistIterator iter;
    multilistIteratorInit((multilist *)ml, state, &iter, true,
                          true); /* forward, readOnly */

    multilistEntry entry;
    while (multilistNext(&iter, &entry)) {
        flexPushByType(&f, &entry.box, FLEX_ENDPOINT_TAIL);
    }

    multilistIteratorRelease(&iter);
    mflexStateFree(state[0]);
    mflexStateFree(state[1]);

    *len = flexBytes(f);
    uint8_t *buf = zmalloc(*len);
    memcpy(buf, f, *len);
    flexFree(f);
    return buf;
}

static void *multilistPersistRestore(const uint8_t *data, size_t len) {
    /* Restore the flex */
    flex *f = zmalloc(len);
    memcpy(f, data, len);

    /* Create multilist and populate from flex */
    multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0); /* default config */
    /* Use thread-local replay state set by persistRecover */
    mflexState *state = persistGetReplayState();
    if (!state) {
        /* If not in replay context, create temporary state */
        state = mflexStateCreate();
        if (!state) {
            multilistFree(ml);
            flexFree(f);
            return NULL;
        }
    }

    /* Iterate through flex and push each element to multilist
     * Note: flexHead returns non-NULL even for empty flex (points to end
     * marker), so we must check count to know when to stop */
    size_t count = flexCount(f);
    flexEntry *fe = flexHead(f);
    for (size_t i = 0; i < count; i++) {
        databox box;
        flexGetByType(fe, &box);
        multilistPushByTypeTail(&ml, state, &box);
        fe = flexNext(f, fe);
    }

    /* Free temporary state if we created one */
    if (state != persistGetReplayState()) {
        mflexStateFree(state);
    }

    flexFree(f);
    return ml;
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
        /* args is const databox* */
        const databox *box = args;
        databoxLinear dl;
        size_t encodedLen;
        const void *encodedVal;
        uint8_t encodedType;
        DATABOX_LINEAR_PARTS_ENCODE(box, encodedLen, encodedVal, encodedType,
                                    &dl);

        *len = 1 + encodedLen;
        uint8_t *buf = zmalloc(*len);
        buf[0] = encodedType;
        memcpy(buf + 1, encodedVal, encodedLen);
        return buf;
    }

    case PERSIST_OP_POP_HEAD:
    case PERSIST_OP_POP_TAIL:
    case PERSIST_OP_CLEAR:
        /* No arguments needed */
        *len = 0;
        return NULL;

    case PERSIST_OP_DELETE_AT:
    case PERSIST_OP_REPLACE: {
        /* args is const databox*[] with [arg0, arg1] */
        if (argc < 2) {
            *len = 0;
            return NULL;
        }
        const databox *const *boxes = args;

        /* Calculate total length: 1 byte argc + encoded boxes */
        size_t totalLen = 1;
        databoxLinear dls[2];

        for (size_t i = 0; i < 2; i++) {
            size_t encodedLen;
            const void *encodedVal;
            uint8_t encodedType;
            DATABOX_LINEAR_PARTS_ENCODE(boxes[i], encodedLen, encodedVal,
                                        encodedType, &dls[i]);
            (void)encodedVal;  /* Set by macro but only encodedLen is needed */
            (void)encodedType; /* Set by macro but only encodedLen is needed */
            totalLen += 1 + encodedLen;
        }

        *len = totalLen;
        uint8_t *buf = zmalloc(totalLen);
        buf[0] = 2; /* argc */

        size_t offset = 1;
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

    default:
        *len = 0;
        return NULL;
    }
}

static bool multilistPersistApplyOp(void *structure, persistOp op,
                                    const uint8_t *data, size_t len) {
    multilist **ml = structure;
    /* Use thread-local replay state set by persistReplayWAL */
    mflexState *state = persistGetReplayState();

    switch (op) {
    case PERSIST_OP_PUSH_HEAD:
    case PERSIST_OP_PUSH_TAIL: {
        if (len < 1) {
            return false;
        }

        databox box;
        DATABOX_LINEAR_PARTS_DECODE(data[0], data + 1, len - 1, &box);

        if (op == PERSIST_OP_PUSH_HEAD) {
            multilistPushByTypeHead(ml, state, &box);
        } else {
            multilistPushByTypeTail(ml, state, &box);
        }
        return true;
    }

    case PERSIST_OP_POP_HEAD: {
        databox got;
        return multilistPop(ml, state, &got, false);
    }

    case PERSIST_OP_POP_TAIL: {
        databox got;
        return multilistPop(ml, state, &got, true);
    }

    case PERSIST_OP_DELETE_AT: {
        /* Decode [start, count] */
        if (len < 2) {
            return false;
        }

        const uint8_t *p = data;
        const uint8_t *end = data + len;

        /* Decode argc */
        uint8_t argc = *p++;
        if (argc < 2) {
            return false;
        }

        /* Decode start */
        if (p >= end) {
            return false;
        }
        uint8_t type0 = *p++;
        size_t valueLen0 = databoxLinearTypeValueLen(type0);
        if (p + valueLen0 > end) {
            return false;
        }
        databox startBox;
        DATABOX_LINEAR_PARTS_DECODE(type0, p, valueLen0, &startBox);
        p += valueLen0;

        /* Decode count */
        if (p >= end) {
            return false;
        }
        uint8_t type1 = *p++;
        size_t valueLen1 = databoxLinearTypeValueLen(type1);
        if (p + valueLen1 > end) {
            return false;
        }
        databox countBox;
        DATABOX_LINEAR_PARTS_DECODE(type1, p, valueLen1, &countBox);

        return multilistDelRange(ml, state, startBox.data.i, countBox.data.i);
    }

    case PERSIST_OP_REPLACE: {
        /* Decode [index, value] */
        if (len < 2) {
            return false;
        }

        const uint8_t *p = data;
        const uint8_t *end = data + len;

        /* Decode argc */
        uint8_t argc = *p++;
        if (argc < 2) {
            return false;
        }

        /* Decode index */
        if (p >= end) {
            return false;
        }
        uint8_t type0 = *p++;
        size_t valueLen0 = databoxLinearTypeValueLen(type0);
        if (p + valueLen0 > end) {
            return false;
        }
        databox indexBox;
        DATABOX_LINEAR_PARTS_DECODE(type0, p, valueLen0, &indexBox);
        p += valueLen0;

        /* Decode value */
        if (p >= end) {
            return false;
        }
        uint8_t type1 = *p++;
        size_t valueLen1 = databoxLinearTypeValueLen(type1);
        if (p + valueLen1 > end) {
            return false;
        }
        databox valueBox;
        DATABOX_LINEAR_PARTS_DECODE(type1, p, valueLen1, &valueBox);

        return multilistReplaceByTypeAtIndex(
            ml, state, (mlNodeId)indexBox.data.i, &valueBox);
    }

    case PERSIST_OP_CLEAR:
        multilistFree(*ml);
        *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
        return true;

    default:
        return false;
    }
}

static void multilistPersistFree(void *structure) {
    multilistFree(structure);
}

const persistOps persistOpsMultilist = {
    .type = PERSIST_TYPE_MULTILIST,
    .name = "multilist",
    .snapshot = multilistPersistSnapshot,
    .restore = multilistPersistRestore,
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

static uint8_t *multidictPersistSnapshot(const void *structure, size_t *len) {
    const multidict *d = structure;

    /* Create a flex to hold all key-value pairs */
    flex *f = flexNew();

    /* Iterate through all entries and add to flex as [key, val, key, val, ...]
     */
    multidictIterator iter;
    if (multidictIteratorInit((multidict *)d, &iter)) {
        multidictEntry entry;
        while (multidictIteratorNext(&iter, &entry)) {
            flexPushByType(&f, &entry.key, FLEX_ENDPOINT_TAIL);
            flexPushByType(&f, &entry.val, FLEX_ENDPOINT_TAIL);
        }
        multidictIteratorRelease(&iter);
    }

    *len = flexBytes(f);
    uint8_t *buf = zmalloc(*len);
    memcpy(buf, f, *len);
    flexFree(f);
    return buf;
}

static void *multidictPersistRestore(const uint8_t *data, size_t len) {
    /* Restore the flex */
    flex *f = zmalloc(len);
    memcpy(f, data, len);

    /* Create multidict with default type and class */
    multidictClass *qdc = multidictDefaultClassNew();
    multidict *d = multidictNew(&multidictTypeExactKey, qdc, 0);

    /* Iterate through flex and add each key-value pair
     * Note: flexHead returns non-NULL even for empty flex,
     * so we must check count */
    size_t count = flexCount(f);
    flexEntry *fe = flexHead(f);
    for (size_t i = 0; i + 1 < count; i += 2) {
        databox key, val;

        /* Get key */
        flexGetByType(fe, &key);
        fe = flexNext(f, fe);

        /* Get value */
        flexGetByType(fe, &val);
        fe = flexNext(f, fe);

        /* Add to multidict */
        multidictAdd(d, &key, &val);
    }

    flexFree(f);
    return d;
}

static size_t multidictPersistCount(const void *structure) {
    return multidictCount(structure);
}

static size_t multidictPersistEstimateSize(const void *structure) {
    return multidictBytes((multidict *)structure);
}

static uint8_t *multidictPersistEncodeOp(persistOp op, const void *args,
                                         size_t argc, size_t *len) {
    switch (op) {
    case PERSIST_OP_INSERT:
    case PERSIST_OP_REPLACE: {
        /* args is const databox*[] with [key, val] */
        if (argc < 2) {
            *len = 0;
            return NULL;
        }
        const databox *const *boxes = args;

        /* Encode: [keyType][keyData][valType][valData] */
        databoxLinear dlKey, dlVal;
        size_t keyLen, valLen;
        const void *keyVal, *valVal;
        uint8_t keyType, valType;

        DATABOX_LINEAR_PARTS_ENCODE(boxes[0], keyLen, keyVal, keyType, &dlKey);
        DATABOX_LINEAR_PARTS_ENCODE(boxes[1], valLen, valVal, valType, &dlVal);

        *len = 2 + keyLen + valLen; /* 2 type bytes + data */
        uint8_t *buf = zmalloc(*len);
        size_t offset = 0;
        buf[offset++] = keyType;
        memcpy(buf + offset, keyVal, keyLen);
        offset += keyLen;
        buf[offset++] = valType;
        memcpy(buf + offset, valVal, valLen);
        return buf;
    }

    case PERSIST_OP_DELETE: {
        /* args is const databox* (key only) */
        const databox *key = args;
        databoxLinear dl;
        size_t encodedLen;
        const void *encodedVal;
        uint8_t encodedType;
        DATABOX_LINEAR_PARTS_ENCODE(key, encodedLen, encodedVal, encodedType,
                                    &dl);

        *len = 1 + encodedLen;
        uint8_t *buf = zmalloc(*len);
        buf[0] = encodedType;
        memcpy(buf + 1, encodedVal, encodedLen);
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

static bool multidictPersistApplyOp(void *structure, persistOp op,
                                    const uint8_t *data, size_t len) {
    multidict **d = structure;

    switch (op) {
    case PERSIST_OP_INSERT:
    case PERSIST_OP_REPLACE: {
        if (len < 2) {
            return false;
        }

        /* Decode key */
        databox key;
        size_t offset = 0;
        uint8_t keyType = data[offset++];
        size_t keyLen = databoxLinearTypeValueLen(keyType);

        if (offset + keyLen > len - 1) {
            return false;
        }
        DATABOX_LINEAR_PARTS_DECODE(keyType, data + offset, keyLen, &key);
        offset += keyLen;

        /* Decode value */
        databox val;
        uint8_t valType = data[offset++];
        size_t valLen = databoxLinearTypeValueLen(valType);

        if (offset + valLen > len) {
            return false;
        }
        DATABOX_LINEAR_PARTS_DECODE(valType, data + offset, valLen, &val);

        if (op == PERSIST_OP_REPLACE) {
            multidictReplace(*d, &key, &val);
        } else {
            multidictAdd(*d, &key, &val);
        }
        return true;
    }

    case PERSIST_OP_DELETE: {
        if (len < 1) {
            return false;
        }

        databox key;
        DATABOX_LINEAR_PARTS_DECODE(data[0], data + 1, len - 1, &key);
        multidictDelete(*d, &key);
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
    multidictFree(structure);
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

const persistOps persistOpsMultiarray = {
    .type = PERSIST_TYPE_MULTIARRAY,
    .name = "multiarray",
    .snapshot = NULL,
    .restore = NULL,
    .count = NULL,
    .estimateSize = NULL,
    .encodeOp = NULL,
    .applyOp = NULL,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = NULL,
    .free = NULL,
};

/* ============================================================================
 * Structure-Specific Operations: multilru
 * ============================================================================
 */
#include "../deps/varint/src/varintTagged.h"
#include "multilru.h"

/* Snapshot format (using flex):
 * - Config:
 * [maxLevels][enableWeights][policy][evictStrategy][maxCount][maxWeight]
 * - Entries: For each active entry in handle order:
 *   [handle (size_t)][level (uint8_t)][weight (uint64_t, if weights enabled)]
 */

static uint8_t *multilruPersistSnapshot(const void *structure, size_t *len) {
    const multilru *mlru = structure;

    flex *f = flexNew();

    /* Handle NULL (empty LRU) */
    if (mlru) {
        /* Get stats */
        multilruStats stats;
        multilruGetStats(mlru, &stats);

        /* Check if weights are enabled using the new API */
        bool enableWeights = multilruHasWeights(mlru);

        /* Store configuration */
        databox dbMaxLevels = databoxNewUnsigned(stats.maxLevels);
        databox dbEnableWeights = databoxNewUnsigned(enableWeights ? 1 : 0);
        databox dbPolicy =
            databoxNewUnsigned(0); /* Policy not exposed in API, use 0 */
        databox dbEvictStrategy =
            databoxNewUnsigned(0); /* Strategy not exposed, use 0 */
        databox dbMaxCount = databoxNewUnsigned(stats.maxCount);
        databox dbMaxWeight = databoxNewUnsigned(stats.maxWeight);
        databox dbNextFresh = databoxNewUnsigned(
            stats.nextFresh); /* Track next handle to preserve gaps */

        flexPushByType(&f, &dbMaxLevels, FLEX_ENDPOINT_TAIL);
        flexPushByType(&f, &dbEnableWeights, FLEX_ENDPOINT_TAIL);
        flexPushByType(&f, &dbPolicy, FLEX_ENDPOINT_TAIL);
        flexPushByType(&f, &dbEvictStrategy, FLEX_ENDPOINT_TAIL);
        flexPushByType(&f, &dbMaxCount, FLEX_ENDPOINT_TAIL);
        flexPushByType(&f, &dbMaxWeight, FLEX_ENDPOINT_TAIL);
        flexPushByType(&f, &dbNextFresh, FLEX_ENDPOINT_TAIL);

        /* Store entries - iterate through all populated handles */
        for (size_t handle = 1; handle < stats.nextFresh; handle++) {
            if (multilruIsPopulated(mlru, handle)) {
                databox dbHandle = databoxNewUnsigned(handle);
                databox dbLevel =
                    databoxNewUnsigned(multilruGetLevel(mlru, handle));

                flexPushByType(&f, &dbHandle, FLEX_ENDPOINT_TAIL);
                flexPushByType(&f, &dbLevel, FLEX_ENDPOINT_TAIL);

                /* ALWAYS store weight if weights are enabled (even if zero) to
                 * maintain consistent field count */
                if (enableWeights) {
                    databox dbWeight =
                        databoxNewUnsigned(multilruGetWeight(mlru, handle));
                    flexPushByType(&f, &dbWeight, FLEX_ENDPOINT_TAIL);
                }
            }
        }
    }

    *len = flexBytes(f);
    uint8_t *buf = zmalloc(*len);
    memcpy(buf, f, *len);
    flexFree(f);
    return buf;
}

static void *multilruPersistRestore(const uint8_t *data, size_t len) {
    /* Restore flex from data */
    flex *f = zmalloc(len);
    memcpy(f, data, len);

    size_t count = flexCount(f);
    if (count < 6) {
        /* Invalid: need at least config fields */
        flexFree(f);
        return NULL;
    }

    /* Read configuration */
    flexEntry *fe = flexHead(f);
    databox db;

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
    multilru *mlru = multilruNewWithConfig(&config);
    if (!mlru) {
        flexFree(f);
        return NULL;
    }

    /* Disable auto-eviction during restore */
    multilruSetAutoEvict(mlru, false);

    /* Restore entries */
    size_t fieldsPerEntry = enableWeights ? 3 : 2;
    size_t numEntries = (count - configFields) / fieldsPerEntry;

    /* Track which handles are real (from snapshot) vs dummy (gap fillers) */
    bool *isRealEntry = hasNextFresh && targetNextFresh > 0
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

    /* Free tracking array */
    if (isRealEntry) {
        zfree(isRealEntry);
    }

    /* Re-enable auto-eviction */
    multilruSetAutoEvict(mlru, true);

    flexFree(f);
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

static uint8_t *multiOrderedSetPersistSnapshot(const void *structure,
                                               size_t *len) {
    const multiOrderedSet *mos = structure;

    /* Create a flex to hold all (score, member) pairs */
    flex *f = flexNew();

    /* Handle NULL (empty set) */
    if (mos) {
        /* Iterate through all entries in score order */
        mosIterator iter;
        multiOrderedSetIteratorInit(mos, &iter, true); /* forward */

        databox member, score;
        while (multiOrderedSetIteratorNext(&iter, &member, &score)) {
            /* Store score first, then member (matches sorted order) */
            flexPushByType(&f, &score, FLEX_ENDPOINT_TAIL);
            flexPushByType(&f, &member, FLEX_ENDPOINT_TAIL);
        }

        multiOrderedSetIteratorRelease(&iter);
    }

    *len = flexBytes(f);
    uint8_t *buf = zmalloc(*len);
    memcpy(buf, f, *len);
    flexFree(f);
    return buf;
}

static void *multiOrderedSetPersistRestore(const uint8_t *data, size_t len) {
    /* Restore flex from data */
    flex *f = zmalloc(len);
    memcpy(f, data, len);

    /* Create multiOrderedSet (starts as Small tier even if empty) */
    multiOrderedSet *mos = multiOrderedSetNew();

    /* Iterate through (score, member) pairs in the flex */
    size_t count = flexCount(f);
    if (count % 2 != 0) {
        /* Invalid: should have even number of elements (score, member pairs) */
        flexFree(f);
        return NULL;
    }

    flexEntry *fe = flexHead(f);
    for (size_t i = 0; i < count / 2 && fe; i++) {
        databox score, member;

        /* Read score */
        flexGetByType(fe, &score);
        fe = flexNext(f, fe);
        if (!fe) {
            break;
        }

        /* Read member */
        flexGetByType(fe, &member);
        fe = flexNext(f, fe);

        /* Add to multiOrderedSet */
        multiOrderedSetAdd(&mos, &score, &member);
    }

    flexFree(f);

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

    TEST("checksum32 basic") {
        const char *data = "hello world";
        uint32_t c1 = persistChecksum32(data, strlen(data));
        uint32_t c2 = persistChecksum32(data, strlen(data));
        if (c1 != c2) {
            ERRR("Same data should produce same checksum");
        }

        uint32_t c3 = persistChecksum32("hello world!", 12);
        if (c1 == c3) {
            ERRR("Different data should produce different checksum");
        }
    }

    TEST("checksum64 basic") {
        const char *data = "hello world";
        uint64_t c1 = persistChecksum64(data, strlen(data));
        uint64_t c2 = persistChecksum64(data, strlen(data));
        if (c1 != c2) {
            ERRR("Same data should produce same checksum");
        }
    }

    TEST("checksum types - XXH32/64/128") {
        const char *data = "test data for checksums";
        size_t len = strlen(data);

        /* Test XXH32 */
        persistChecksumValue cs32;
        persistChecksumCompute(PERSIST_CHECKSUM_XXHASH32, data, len, &cs32);
        if (cs32.type != PERSIST_CHECKSUM_XXHASH32) {
            ERRR("XXH32 type mismatch");
        }
        if (cs32.len != 4) {
            ERR("XXH32 should be 4 bytes, got %u", cs32.len);
        }

        /* Test XXH64 */
        persistChecksumValue cs64;
        persistChecksumCompute(PERSIST_CHECKSUM_XXHASH64, data, len, &cs64);
        if (cs64.type != PERSIST_CHECKSUM_XXHASH64) {
            ERRR("XXH64 type mismatch");
        }
        if (cs64.len != 8) {
            ERR("XXH64 should be 8 bytes, got %u", cs64.len);
        }

        /* Test XXH128 */
        persistChecksumValue cs128;
        persistChecksumCompute(PERSIST_CHECKSUM_XXHASH128, data, len, &cs128);
        if (cs128.type != PERSIST_CHECKSUM_XXHASH128) {
            ERRR("XXH128 type mismatch");
        }
        if (cs128.len != 16) {
            ERR("XXH128 should be 16 bytes, got %u", cs128.len);
        }

        /* Verify different algorithms produce different values */
        if (cs32.value.u32 == (uint32_t)cs64.value.u64) {
            ERRR("XXH32 and XXH64 should produce different values");
        }
    }

    TEST("checksum equality and verification") {
        const char *data = "verify this data";
        size_t len = strlen(data);

        persistChecksumValue cs1, cs2;
        persistChecksumCompute(PERSIST_CHECKSUM_XXHASH64, data, len, &cs1);
        persistChecksumCompute(PERSIST_CHECKSUM_XXHASH64, data, len, &cs2);

        /* Same data should produce equal checksums */
        if (!persistChecksumEqual(&cs1, &cs2)) {
            ERRR("Equal checksums should match");
        }

        /* Verify should succeed */
        if (!persistChecksumVerify(&cs1, data, len)) {
            ERRR("Valid checksum should verify");
        }

        /* Different data should fail verification */
        const char *otherData = "different data here";
        if (persistChecksumVerify(&cs1, otherData, strlen(otherData))) {
            ERRR("Invalid checksum should not verify");
        }

        /* Different checksum types should not be equal */
        persistChecksumValue cs32;
        persistChecksumCompute(PERSIST_CHECKSUM_XXHASH32, data, len, &cs32);
        if (persistChecksumEqual(&cs1, &cs32)) {
            ERRR("Different checksum types should not be equal");
        }
    }

    TEST("snapshot with different checksum types") {
        const persistChecksum types[] = {
            PERSIST_CHECKSUM_XXHASH32,
            PERSIST_CHECKSUM_XXHASH64,
            PERSIST_CHECKSUM_XXHASH128,
        };

        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            flex *f = flexNew();
            flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);
            flexPushSigned(&f, 999, FLEX_ENDPOINT_TAIL);
            flexPushBytes(&f, "test", 4, FLEX_ENDPOINT_TAIL);

            persistConfig config = persistDefaultConfig();
            config.checksumType = types[i];

            persist *p = persistCreate(&persistOpsFlex, &config);
            persistStore *snapStore = persistStoreMemory(0);
            persistStore *walStore = persistStoreMemory(0);
            persistAttachSnapshot(p, snapStore);
            persistAttachWAL(p, walStore);

            /* Snapshot with this checksum type */
            if (!persistSnapshot(p, f)) {
                ERR("Snapshot failed for checksum type %d", types[i]);
            }

            /* Verify snapshot */
            if (!persistVerifySnapshot(snapStore)) {
                ERR("Snapshot verification failed for checksum type %d",
                    types[i]);
            }

            /* Restore and verify */
            flex *restored = persistRestore(p);
            if (!restored) {
                ERR("Restore failed for checksum type %d", types[i]);
            }

            if (flexCount(restored) != flexCount(f)) {
                ERR("Restored count mismatch for checksum type %d", types[i]);
            }

            flexFree(f);
            flexFree(restored);
            persistClose(p);
        }
    }

    TEST("checksum performance") {
        /* Test different data sizes */
        const size_t sizes[] = {
            64,               /* Small: header-sized */
            1024,             /* 1KB */
            64 * 1024,        /* 64KB */
            1024 * 1024,      /* 1MB */
            16 * 1024 * 1024, /* 16MB */
        };

        const char *sizeNames[] = {"64B", "1KB", "64KB", "1MB", "16MB"};

        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            size_t size = sizes[s];
            uint8_t *data = zmalloc(size);

            /* Fill with pseudo-random data */
            for (size_t i = 0; i < size; i++) {
                data[i] = (uint8_t)(i * 7919 + 104729);
            }

            printf("\n=== %s data ===\n", sizeNames[s]);

            /* Adaptive iteration count based on data size */
            const size_t iterations = size < 1024 * 1024 ? 10000 : 100;

            /* Benchmark XXH32 */
            {
                printf("  XXH32:  ");
                PERF_TIMERS_SETUP;

                for (size_t i = 0; i < iterations; i++) {
                    persistChecksumValue cs;
                    persistChecksumCompute(PERSIST_CHECKSUM_XXHASH32, data,
                                           size, &cs);
                }

                PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "checksums");
            }

            /* Benchmark XXH64 */
            {
                printf("  XXH64:  ");
                PERF_TIMERS_SETUP;

                for (size_t i = 0; i < iterations; i++) {
                    persistChecksumValue cs;
                    persistChecksumCompute(PERSIST_CHECKSUM_XXHASH64, data,
                                           size, &cs);
                }

                PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "checksums");
            }

            /* Benchmark XXH128 */
            {
                printf("  XXH128: ");
                PERF_TIMERS_SETUP;

                for (size_t i = 0; i < iterations; i++) {
                    persistChecksumValue cs;
                    persistChecksumCompute(PERSIST_CHECKSUM_XXHASH128, data,
                                           size, &cs);
                }

                PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "checksums");
            }

            zfree(data);
        }

        printf("\n");
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
            ERR("After truncate, size should be 5, got %lld",
                (long long)store->size(store->ctx));
        }

        store->close(store->ctx);
        zfree(store);
    }

    /* ================================================================
     * Flex Persistence Tests
     * ================================================================ */

    TEST("flex snapshot and restore") {
        flex *f = flexNew();
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
            ERR("First element mismatch: %lld vs %lld", (long long)box1.data.i,
                (long long)box2.data.i);
        }

        fe1 = flexNext(f, fe1);
        fe2 = flexNext(restored, fe2);
        flexGetByType(fe1, &box1);
        flexGetByType(fe2, &box2);
        if (box1.data.i != box2.data.i || box1.data.i != -100) {
            ERR("Second element mismatch: %lld vs %lld", (long long)box1.data.i,
                (long long)box2.data.i);
        }

        flexFree(f);
        flexFree(restored);
        persistClose(p);
    }

    TEST("flex WAL operations") {
        flex *f = flexNew();

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
            ERR("Expected 3 WAL entries, got %llu",
                (unsigned long long)stats.walEntries);
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
            ERR("First should be -50, got %lld", (long long)box.data.i);
        }

        fe = flexNext(recovered, fe);
        flexGetByType(fe, &box);
        if (box.data.i != 100) {
            ERR("Second should be 100, got %lld", (long long)box.data.i);
        }

        fe = flexNext(recovered, fe);
        flexGetByType(fe, &box);
        if (box.data.i != 200) {
            ERR("Third should be 200, got %lld", (long long)box.data.i);
        }

        flexFree(f);
        flexFree(recovered);
        persistClose(p);
    }

    TEST("flex compaction") {
        flex *f = flexNew();
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
            ERR("WAL should be small after compaction, got %lld",
                (long long)walSize);
        }

        /* Recover should still work */
        flex *recovered = persistRecover(p);
        if (!recovered) {
            ERRR("Recovery after compaction failed");
        }

        if (flexCount(recovered) != 4) {
            ERR("Expected 4 elements, got %zu", flexCount(recovered));
        }

        flexFree(f);
        flexFree(recovered);
        persistClose(p);
    }

    /* ================================================================
     * Intset Persistence Tests
     * ================================================================ */

    TEST("intset snapshot and restore") {
        intset *is = intsetNew();
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

        intsetFree(is);
        intsetFree(restored);
        persistClose(p);
    }

    TEST("intset WAL operations") {
        intset *is = intsetNew();

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

        intsetFree(is);
        intsetFree(recovered);
        persistClose(p);
    }

    /* ================================================================
     * Multimap Persistence Tests
     * ================================================================ */

    TEST("multimap snapshot and restore") {
        multimap *m = multimapNew(2); /* key + value */

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
            ERR("key1 value should be 100, got %lld",
                (long long)foundVal.data.i);
        }

        if (!multimapLookup(restored, &key2, foundPtrs)) {
            ERRR("key2 not found");
        }
        if (foundVal.data.i != 200) {
            ERR("key2 value should be 200, got %lld",
                (long long)foundVal.data.i);
        }

        multimapFree(m);
        multimapFree(restored);
        persistClose(p);
    }

    /* ================================================================
     * Verification Tests
     * ================================================================ */

    TEST("verify snapshot integrity") {
        flex *f = flexNew();
        flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);

        persist *p = persistCreate(&persistOpsFlex, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, f);

        if (!persistVerifySnapshot(store)) {
            ERRR("Valid snapshot should verify");
        }

        /* Corrupt the data */
        size_t len;
        const uint8_t *buf = persistStoreMemoryBuffer(store, &len);
        if (len > 40) {
            /* Modify a byte in the body */
            ((uint8_t *)buf)[40] ^= 0xFF;
        }

        if (persistVerifySnapshot(store)) {
            ERRR("Corrupted snapshot should fail verification");
        }

        flexFree(f);
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
        flex *f = flexNew();

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

        flexFree(f);
        flexFree(restored);
        persistClose(p);
    }

    TEST("intset round-trip with edge cases") {
        intset *is = intsetNew();
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
                ERR("Missing value: %lld", (long long)values[i]);
            }
        }

        intsetFree(is);
        intsetFree(restored);
        persistClose(p);
    }

    /* ================================================================
     * Multilist Persistence Tests
     * ================================================================ */

    TEST("multilist snapshot and restore") {
        multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
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
                ERR("First element should be 0, got %lld",
                    (long long)entry.box.data.i);
            }
        } else {
            ERRR("Could not get first element");
        }

        if (multilistIndex(restored, state2, -1, &entry, true)) {
            if (entry.box.data.i != 490) {
                ERR("Last element should be 490, got %lld",
                    (long long)entry.box.data.i);
            }
        } else {
            ERRR("Could not get last element");
        }

        multilistFree(ml);
        multilistFree(restored);
        persistClose(p);
    }

    TEST("multilist WAL operations") {
        multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
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

        multilistFree(ml);
        multilistFree(recovered);
        persistClose(p);
    }

    /* ================================================================
     * Multidict Persistence Tests
     * ================================================================ */

    TEST("multidict snapshot and restore") {
        multidictClass *qdc = multidictDefaultClassNew();
        multidict *d = multidictNew(&multidictTypeExactKey, qdc, 0);

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
            ERR("Count mismatch: %llu vs %llu",
                (unsigned long long)multidictCount(restored),
                (unsigned long long)multidictCount(d));
        }

        /* Verify some entries */
        for (int i = 0; i < 20; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val;
            if (!multidictFind(restored, &key, &val)) {
                ERR("Key %d not found", i);
            } else if (val.data.i != i * 1000) {
                ERR("Key %d: expected value %d, got %lld", i, i * 1000,
                    (long long)val.data.i);
            }
        }

        multidictFree(d);
        multidictFree(restored);
        persistClose(p);
    }

    TEST("WAL replay idempotence") {
        /* Replaying the same WAL twice should give same result */
        flex *f1 = flexNew();
        flex *f2 = flexNew();

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

        flexFree(f1);
        flexFree(f2);
        flexFree(r1);
        flexFree(r2);
        persistClose(p);
    }

    /* ================================================================
     * Comprehensive Flex Tests
     * ================================================================ */

    TEST("flex: empty structure round-trip") {
        flex *f = flexNew();

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

        flexFree(f);
        flexFree(restored);
        persistClose(p);
    }

    TEST("flex: mixed types round-trip") {
        flex *f = flexNew();

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

        flexFree(f);
        flexFree(restored);
        persistClose(p);
    }

    TEST("flex: large structure round-trip") {
        flex *f = flexNew();

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

        flexFree(f);
        flexFree(restored);
        persistClose(p);
    }

    TEST("flex: WAL with mixed operations") {
        flex *f = flexNew();

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
                ERR("Element %d: expected %lld, got %lld", i,
                    (long long)expected[i], (long long)box.data.i);
            }
            fe = flexNext(recovered, fe);
        }

        flexFree(f);
        flexFree(recovered);
        persistClose(p);
    }

    /* ================================================================
     * Comprehensive Intset Tests
     * ================================================================ */

    TEST("intset: empty structure round-trip") {
        intset *is = intsetNew();

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

        intsetFree(is);
        intsetFree(restored);
        persistClose(p);
    }

    TEST("intset: small tier values only (int16)") {
        intset *is = intsetNew();
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

        intsetFree(is);
        intsetFree(restored);
        persistClose(p);
    }

    TEST("intset: medium tier values (int16 + int32)") {
        intset *is = intsetNew();
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
                ERR("Missing medium value: %lld", (long long)values[i]);
            }
        }

        intsetFree(is);
        intsetFree(restored);
        persistClose(p);
    }

    TEST("intset: full tier values (int16 + int32 + int64)") {
        intset *is = intsetNew();
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
                ERR("Missing full tier value: %lld", (long long)values[i]);
            }
        }

        intsetFree(is);
        intsetFree(restored);
        persistClose(p);
    }

    TEST("intset: large count round-trip") {
        intset *is = intsetNew();
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

        intsetFree(is);
        intsetFree(restored);
        persistClose(p);
    }

    TEST("intset: WAL insert and delete") {
        intset *is = intsetNew();

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

        intsetFree(is);
        intsetFree(recovered);
        persistClose(p);
    }

    /* ================================================================
     * Comprehensive Multimap Tests
     * ================================================================ */

    TEST("multimap: empty structure round-trip") {
        multimap *m = multimapNew(2);

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

        multimapFree(m);
        multimapFree(restored);
        persistClose(p);
    }

    TEST("multimap: various element widths") {
        /* Test with 2, 3, 4, 5 element entries */
        for (uint32_t width = 2; width <= 5; width++) {
            multimap *m = multimapNew(width);

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

            multimapFree(m);
            multimapFree(restored);
            persistClose(p);
        }
    }

    TEST("multimap: mixed key types") {
        multimap *m = multimapNew(2);

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

        multimapFree(m);
        multimapFree(restored);
        persistClose(p);
    }

    TEST("multimap: large structure triggers tier transitions") {
        multimap *m = multimapNew(2);

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
                ERR("Key %d: expected %d, got %lld", i, i * 7 - 1000,
                    (long long)val.data.i);
            }
        }

        multimapFree(m);
        multimapFree(restored);
        persistClose(p);
    }

    /* ================================================================
     * Comprehensive Multilist Tests
     * ================================================================ */

    TEST("multilist: empty structure round-trip") {
        multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);

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

        multilistFree(ml);
        multilistFree(restored);
        persistClose(p);
    }

    TEST("multilist: various element types") {
        multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
        mflexState *state = mflexStateCreate();

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

        mflexStateFree(state);
        multilistFree(ml);
        multilistFree(restored);
        persistClose(p);
    }

    TEST("multilist: large multi-node structure") {
        multilist *ml = multilistNew(FLEX_CAP_LEVEL_512, 0);
        mflexState *state = mflexStateCreate();

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
        mflexState *s2 = mflexStateCreate();
        for (int i = 0; i < 10000; i += 1000) {
            multilistEntry entry;
            if (!multilistIndex(restored, s2, i, &entry, true)) {
                ERR("Failed to get index %d", i);
            } else if (entry.box.data.i != i) {
                ERR("Index %d: expected %d, got %lld", i, i,
                    (long long)entry.box.data.i);
            }
        }

        mflexStateFree(state);
        mflexStateFree(s2);
        multilistFree(ml);
        multilistFree(restored);
        persistClose(p);
    }

    /* ================================================================
     * Comprehensive Multidict Tests
     * ================================================================ */

    TEST("multidict: empty structure round-trip") {
        multidictClass *qdc = multidictDefaultClassNew();
        multidict *d = multidictNew(&multidictTypeExactKey, qdc, 0);

        persist *p = persistCreate(&persistOpsMultidict, NULL);
        persistStore *store = persistStoreMemory(0);
        persistAttachSnapshot(p, store);
        persistSnapshot(p, d);

        multidict *restored = persistRestore(p);
        if (!restored) {
            ERRR("Empty multidict restore failed");
        }
        if (multidictCount(restored) != 0) {
            ERR("Empty multidict should have 0 elements, got %llu",
                (unsigned long long)multidictCount(restored));
        }

        multidictFree(d);
        multidictFree(restored);
        persistClose(p);
    }

    TEST("multidict: large hash table") {
        multidictClass *qdc = multidictDefaultClassNew();
        multidict *d = multidictNew(&multidictTypeExactKey, qdc, 0);

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
            ERR("Expected 1000 entries, got %llu",
                (unsigned long long)multidictCount(restored));
        }

        /* Verify all entries */
        for (int i = 0; i < 1000; i++) {
            databox key = DATABOX_SIGNED(i * 17);
            databox val;
            if (!multidictFind(restored, &key, &val)) {
                ERR("Missing key %d", i * 17);
            } else if (val.data.i != i) {
                ERR("Key %d: expected value %d, got %lld", i * 17, i,
                    (long long)val.data.i);
            }
        }

        multidictFree(d);
        multidictFree(restored);
        persistClose(p);
    }

    /* ================================================================
     * Fuzz Tests
     * ================================================================ */

    TEST("FUZZ: flex random operations") {
        uint32_t seed = 12345;
        flex *f = flexNew();

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

        flexFree(f);
        flexFree(restored);
        persistClose(p);
    }

    TEST("FUZZ: intset random values") {
        uint64_t seed = 67890;
        intset *is = intsetNew();
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
                ERR("Fuzz intset missing value: %lld",
                    (long long)addedValues[i]);
            }
        }

        intsetFree(is);
        intsetFree(restored);
        persistClose(p);
    }

    TEST("FUZZ: multimap random entries") {
        uint32_t seed = 11111;
        multimap *m = multimapNew(3); /* key, val1, val2 */

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

        multimapFree(m);
        multimapFree(restored);
        persistClose(p);
    }

    TEST("FUZZ: multilist random push/pop") {
        uint32_t seed = 22222;
        multilist *ml = multilistNew(FLEX_CAP_LEVEL_1024, 0);
        mflexState *state = mflexStateCreate();

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
                    databox got;
                    multilistPop(&ml, state, &got, FLEX_ENDPOINT_TAIL);
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

        mflexStateFree(state);
        multilistFree(ml);
        multilistFree(restored);
        persistClose(p);
    }

    TEST("FUZZ: WAL replay stress test") {
        flex *f = flexNew();

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

        flexFree(f);
        flexFree(recovered);
        persistClose(p);
    }

    TEST("FUZZ: multiple snapshot-restore cycles") {
        intset *is = intsetNew();
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

        intsetFree(is);
    }

    TEST("FUZZ: compaction preserves data") {
        flex *f = flexNew();

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
                ERR("Post-compaction element %d: expected %d, got %lld", i, i,
                    (long long)box.data.i);
            }
            fe = flexNext(afterCompact, fe);
        }

        flexFree(f);
        flexFree(withWal);
        flexFree(afterCompact);
        persistClose(p);
    }

    TEST("FUZZ: concurrent-like WAL and snapshot operations") {
        multilist *ml = multilistNew(FLEX_CAP_LEVEL_512, 0);
        mflexState *state = mflexStateCreate();

        persist *p = persistCreate(&persistOpsMultilist, NULL);
        persistStore *snapStore = persistStoreMemory(0);
        persistStore *walStore = persistStoreMemory(0);
        persistAttachSnapshot(p, snapStore);
        persistAttachWAL(p, walStore);

        /* Simulate interleaved operations */
        uint32_t seed = 44444;

        for (int phase = 0; phase < 5; phase++) {
            /* Add to structure and snapshot */
            for (int i = 0; i < 20; i++) {
                databox box = DATABOX_SIGNED(phase * 100 + i);
                multilistPushByTypeTail(&ml, state, &box);
            }
            persistSnapshot(p, ml);

            /* Log more operations to WAL */
            for (int i = 0; i < 10; i++) {
                seed = seed * 1103515245 + 12345;
                databox box = DATABOX_SIGNED((int64_t)seed);
                persistLogOp(p, PERSIST_OP_PUSH_TAIL, &box, 1);
            }
            persistSync(p);
        }

        /* Final recovery should have all data */
        multilist *recovered = persistRecover(p);
        if (!recovered) {
            ERRR("Interleaved ops recovery failed");
        }

        /* Note: Each snapshot replaces previous, but WAL accumulates.
         * We have: last snapshot (100 elements) + all WAL ops (5*10=50) */
        if (multilistCount(recovered) != 150) {
            ERR("Interleaved: expected 150, got %zu",
                multilistCount(recovered));
        }

        mflexStateFree(state);
        multilistFree(ml);
        multilistFree(recovered);
        persistClose(p);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
