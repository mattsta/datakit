# Thread Safety

## The rules

1. **No datakit container locks itself.** A multimap, multilist, multiarray, multidict, multilru,
   intset, multiroar, coldmap, multitier, timerWheel or multiTimer is used by one thread at a time, or
   under a lock the caller holds. The few modules that do synchronize are listed
   [below](#what-is-safe-across-threads).
2. **A write can move the handle.** Every call that can change a container's tier or size takes
   `X **` and may replace the pointer (see [Tiered containers](../modules/core/TIERS.md)). Threads must
   read the handle through the same lock that guards the writes, never from a copy taken earlier.
3. **Some reads write.** A call that takes the container as `const` only reads it, so `const` reads
   may run together under a read lock: `multimapLookup`, `multimapIteratorInit` and the multimap
   iterator, `intsetFind`, the `flexGet*` reads and `flexNext`. Every other call needs exclusive access,
   including multidict's reads: each `multidictFind`, `multidictFindNoTouch` and `multidictExists`
   runs one step of the dict's incremental rehash, and with LRU tracking on a find also records the
   use.

## What is safe across threads

| Module                                           | Contract (from its header)                                                                                                                                                                                                                |
| ------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [ownedState](../modules/system/OWNED_STATE.md)   | Single-writer publication: one writer (or one serialized set) replaces a value whole; any number of threads read it without a lock; the replaced value is freed once no reader can hold it.                                               |
| `hmacKeyring`                                    | A `pthread_rwlock`: lookups run concurrently, one writer (install, retire, salt, restore) at a time. A pointer a lookup returns is borrowed until that key is changed; copy (`hmacKeyringGetRawCopy`) to keep key material past a change. |
| `aeadNonceLog`                                   | One mutex guards the log; shard by key id across several logs under heavy contention.                                                                                                                                                     |
| `clusterMailbox`                                 | One thread runs `Seal` and `Ingest`; any thread may read its stats and watermarks at the same time. Create, Free, Snapshot, Restore and ResetStats are serialized by the caller.                                                          |
| [clusterRing](../modules/system/CLUSTER_RING.md) | Placements (`Locate*`, `PlanWrite`, `PlanRead`) run concurrently with each other; a change to the ring needs exclusive access.                                                                                                            |
| [membound](../modules/memory/MEMBOUND.md)        | By tier: STANDARD and ENTERPRISE pools lock internally; a COMPACT pool locks when created with `threadSafe`; a MICRO pool never does.                                                                                                     |
| [fastmutex](../modules/system/FASTMUTEX.md)      | A mutex and condition variable (a subset of pthread's).                                                                                                                                                                                   |
| `hmac` prepared keys                             | A prepared key may be shared by threads as long as none changes it.                                                                                                                                                                       |

### Per-thread state

- **Diagnostics** ([dkDiag](../modules/core/DIAGNOSTICS.md)): each thread reads its own last diagnostic
  and count; the hook (`dkDiagSetHook`) is process-wide and runs on the reporting thread.
- **Envelope verify errors** (`flexEnvelopeLastVerifyError`): kept per thread, until that thread's next
  verify.
- **Random draws** ([dkRandom](../modules/utils/RANDOM.md)): one generator per thread, seeded from the
  OS; a child of `fork()` never repeats its parent's draws.

### Process-wide settings: set once, before the threads start

The allocator configuration (`datakitConfigSet`), the diagnostics hook and print mode, and the backend
overrides used by tests and benchmarks (`rapidhashSetForceImpl`, `blake3SetForceImpl`,
`chacha20SetForceBackend`, `poly1305SetForceBackend`, `aesGcmSetForceBackend`) are plain process
globals: changing one while another thread calls into datakit is a data race.

## Patterns

### One lock

The simplest correct choice, and the right one until a profile shows the lock is contended:

```c
#include <pthread.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static multimap *shared; /* multimapNew(2) at startup */

void sharedPut(const databox *key, const databox *value) {
    const databox *row[2] = {key, value};
    pthread_mutex_lock(&lock);
    multimapInsert(&shared, row); /* may replace 'shared' */
    pthread_mutex_unlock(&lock);
}

bool sharedGet(const databox *key, databox *value) {
    databox *out[1] = {value};
    pthread_mutex_lock(&lock);
    const bool found = multimapLookup(shared, key, out);
    pthread_mutex_unlock(&lock);
    return found; /* a BYTES value points into the map: copy it under the lock */
}
```

A value a lookup fills may point into the container (a byte string is not copied), so copy what you
need before releasing the lock.

### Read-write lock, for `const` reads

Concurrent readers are safe only with reads that write nothing (rule 3). With a multimap:

```c
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static multimap *index; /* multimapNew(2) at startup */

bool indexHas(const databox *key) {
    pthread_rwlock_rdlock(&rw);
    const bool found = multimapExists(index, key);
    pthread_rwlock_unlock(&rw);
    return found;
}

void indexPut(const databox *key, const databox *value) {
    const databox *row[2] = {key, value};
    pthread_rwlock_wrlock(&rw);
    multimapInsert(&index, row);
    pthread_rwlock_unlock(&rw);
}
```

A multidict under the same scheme needs the write lock for its reads too (they step the rehash), so a
read-write lock buys it nothing: use one mutex, or shard.

### One container per thread, merged at the end

Each thread fills its own container with no locking; after the threads are joined, one thread merges
them (for multimaps, iterate each and insert its rows into the result). This is the fastest pattern
when the work partitions cleanly.

### Sharding

Split the key space across N containers, each with its own lock, and pick the shard by a hash of the
key. Contention falls roughly N-fold; operations that span shards (a count, a full scan) take every
lock in a fixed order.

```c
#define SHARDS 16

typedef struct shard {
    pthread_mutex_t lock;
    multimap *map;
} shard;

static shard shards[SHARDS]; /* locks and multimapNew(2) at startup */

static shard *shardFor(const databox *key) {
    const uint64_t h = rapidhash(databoxCBytes(key), databoxLen(key));
    return &shards[h % SHARDS];
}

void shardedPut(const databox *key, const databox *value) {
    shard *s = shardFor(key);
    const databox *row[2] = {key, value};
    pthread_mutex_lock(&s->lock);
    multimapInsert(&s->map, row);
    pthread_mutex_unlock(&s->lock);
}
```

### Publishing a value many threads read

For state read far more often than it changes (configuration, a routing table, a parsed policy), use
[ownedState](../modules/system/OWNED_STATE.md): the writer builds a new value and publishes it, readers
take no lock, and the replaced value is freed only once no reader can still hold it. Swapping a plain
atomic pointer and freeing the old value right away is a use-after-free for any reader that loaded the
pointer just before the swap; ownedState's arena closes that window.

## Checking concurrent code

Build with the `tsan` preset (ThreadSanitizer) and run the tests for the code you changed; datakit's
own atomics carry ordering tags that the `atomicaudit` gate checks. ASan does not see data races.

## See Also

- [ownedState](../modules/system/OWNED_STATE.md) - lock-free publication and deferred reclamation
- [Tiered containers](../modules/core/TIERS.md) - why writes take `X **`
- [multidict](../modules/multi/MULTIDICT.md) - incremental rehashing and LRU tracking
- [Hashes, HMAC, and the flex envelope](../HASH_AND_ENVELOPE.md) - the keyring's lifetime contract
