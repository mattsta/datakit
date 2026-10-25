# persist - Snapshots, Write-Ahead Log and Recovery

## Overview

`persist` makes datakit structures durable.  A structure is saved as a
**snapshot** (its whole state) plus a **write-ahead log** (WAL) of the
operations made since that snapshot.  **Compaction** folds the WAL into a
fresh snapshot, and **recovery** restores the snapshot and replays the WAL.

There are two layers:

- **P wrappers** (`flexP`, `intsetP`, `multimapP`, `multilistP`, `multidictP`,
  `multiOrderedSetP`, `multilruP`, `multiroarP`): a structure plus a
  `persistCtx`.  Each mutator logs the operation, then applies it, so the
  files always describe the structure in memory.  This is the layer most
  code wants.
- **The core** (`persist.h`): a `persist` handle bound to a `persistOps`
  descriptor (how one structure type snapshots, restores, encodes and applies
  operations) and to two `persistStore` backends (where the bytes go).  Use
  it to persist a type of your own or to control the storage directly.

**Headers**: `persist.h`, `persist/persistCtx.h`, `persist/<structure>P.h`

**Source**: `persist.c`, `persist/persistCtx.c`, `persist/<structure>P.c`

## Quick Start (P wrappers)

```c
#include "persist/multimapP.h"

/* New: creates mymap.snap and mymap.wal and writes the first snapshot */
persistCtx *ctx = persistCtxNew("data/mymap", NULL); /* NULL: defaults */
multimapP *m = multimapNewP(ctx, 2);                 /* key + value */

const databox key = databoxNewBytesString("alice");
const databox val = databoxNewSigned(42);
const databox *row[] = {&key, &val};

bool replaced = false;
const persistLogResult r = multimapInsertP(m, row, &replaced);
if (r == PERSIST_LOG_REFUSED) {
    /* nothing was logged and nothing was applied */
}

multimapCloseP(m); /* syncs and frees the multimap */
persistCtxFree(ctx);

/* Later, or after a crash: snapshot + WAL replay */
persistCtx *again = persistCtxOpen("data/mymap", NULL);
multimapP *back = multimapOpenP(again); /* NULL when there is nothing to open */
```

Reads go to the structure directly (`multimapLookupP`, `multimapGetP(m)`
for the underlying multimap, iterators); they never touch the files.

### How a context binds

A `persistCtx` holds one structure's files (`<base>.snap`, `<base>.wal`,
and `<base>.quarantine` once a damaged WAL entry is set aside):

- **To one type.**  The first `*NewP` / `*OpenP` binds the context to its
  structure type; any other type's New, Open, Recover, snapshot or compaction
  on it is refused.
- **To one live wrapper.**  From a successful `*NewP` / `*OpenP` until its
  `*CloseP`, the wrapper holds the context; a second `*NewP` / `*OpenP` on it
  returns NULL.  Close the first, then open again.

Both refusals report a misuse diagnostic (`DK_DIAG_PERSIST_CTX_WRONG_TYPE`,
`DK_DIAG_PERSIST_CTX_CLAIMED`; see [DIAGNOSTICS.md](../core/DIAGNOSTICS.md)).

`persistCtxProbe(basePath)` says what is on disk: `PERSIST_CTX_ABSENT`,
`PERSIST_CTX_PRESENT` (a snapshot an Open recovers) or
`PERSIST_CTX_ORPHAN_WAL` (a WAL without its snapshot, which every Open
treats as absent).  `persistCtxExists` answers whether it is PRESENT and
reports an orphaned WAL as a `DK_DIAG_PERSIST_ORPHAN_WAL` condition.

## Mutation Results

Every P mutator returns a `persistLogResult`:

| Result | Logged | Applied | Meaning |
|---|---|---|---|
| `PERSIST_LOG_REFUSED` (0) | no | no | encoding or the WAL write failed, or the structure refused the operation |
| `PERSIST_LOG_BUFFERED` | yes | yes | in the WAL buffer; the sync policy writes it out later |
| `PERSIST_LOG_DURABLE` | yes | yes | synced to stable storage |
| `PERSIST_LOG_NOT_DURABLE` | yes | yes | the sync the policy asked for failed; durability unknown |
| `PERSIST_LOG_UNCHANGED` | no | no | the operation would change nothing (deleting an absent key, adding a present member, incrementing by 0) |

REFUSED is 0, so `if (result)` reads "the operation happened (or was a
no-op)".  The rule for the core layer is the same: **apply the change exactly
when the result is not REFUSED**.

A wrapper decides, logs and applies a change in the structure's one search
wherever the structure allows it, and a REFUSED result always leaves the
structure as it was:

- **Logged from inside the search** (a refused log declines the change):
  `multidictReplaceP`, `multimapFieldIncrP`, `multiOrderedSetRemoveP`.
- **Located, logged, then changed where found**: `multimapDeleteP`,
  `multimapDeleteFullWidthP`.
- **Applied by the search that decides, undone on a refused log** (the undo
  restores exactly what was there and costs nothing when the log succeeds):
  `multimapInsertIfNotExistsP`, `multiroarBitSetP`, `multiroarRemoveP`,
  `multiroarValueRemoveP`, `intsetAddP`, `intsetRemoveP`.

Durability is **best effort with notification**.  A failed flush or sync
never undoes, blocks or aborts an operation: the result is NOT_DURABLE, the
failure is counted, and the next sync tries again.  Watch the counters:

```c
persistCtxStats s;
multimapGetStatsP(m, &s);
if (s.syncFailures) {
    /* s.lastSyncError: errno of the latest failed sync (0 once one succeeds) */
    /* s.notDurableOps: operations that came back NOT_DURABLE */
}
```

What a mutator used to return (whether a key was replaced, the new counter
value, the handle it allocated) comes back through an optional
out-parameter; pass NULL when you do not need it:

| Wrapper | Out-parameters |
|---|---|
| `intsetAddP` / `intsetRemoveP` | `bool *added` / `bool *removed` |
| `multiroarBitSetP` / `multiroarRemoveP` | `bool *added` / `bool *removed` (positions are `uint64_t`) |
| `multimapInsertP` | `bool *replaced` |
| `multimapFieldIncrP` | `int64_t *newValue` (a refused increment is never logged) |
| `multidictAddP` | `multidictResult *` |
| `multiOrderedSetAddP` | `bool *updated` |
| `multilruInsertP` / `multilruInsertWeightedP` | `multilruPtr *handle` |

## Sync and Compaction

`persistCtxConfig` (from `persistCtxDefaultConfig()`):

- `syncMode`
  - `PERSIST_CTX_SYNC_NONE`: you call `persistCtxSync` (or the wrapper's
    `*SyncP`).
  - `PERSIST_CTX_SYNC_EVERYSEC`: at most one sync a second, triggered by
    operations.  An operation followed by silence stays unsynced until the
    next operation, so call `persistCtxSyncIfDue` from your event loop to
    bound that window.
  - `PERSIST_CTX_SYNC_ALWAYS`: a sync after every operation.
- `compactAfterOps`, `compactAfterBytes`: compact when either is exceeded
  since the last compaction (`compactAfterBytes` measures the live WAL,
  its buffered bytes counted: `persistWALBytes`, and
  `persistCtxStats.bytesSinceCompact`)
  (0 disables that trigger).  `*CompactP` compacts now.
- `walBufferSize`: the WAL write buffer.
- `strictRecovery`: see below.

A compaction writes a new snapshot that records the WAL sequence it includes,
then starts the WAL over.  The file backend publishes the snapshot
atomically (it writes a temporary file, syncs it, renames it over the live
file and syncs the directory), so a crash leaves either the old snapshot or
the new one, never a torn file.  If a crash lands between the new snapshot
and the WAL restart, recovery skips the WAL entries the snapshot already
includes, so nothing is applied twice.

## Recovery

`persistCtxOpen` + `*OpenP` restore the snapshot and replay the WAL.

- **Lenient** (default): an entry that verifies but does not apply is
  skipped and counted (`persistStats.entriesSkipped`).  A torn tail (the
  last entry cut short by a crash) is truncated at the last complete
  entry.  Replay stops at the first entry that does not read and keeps
  what came before; nothing after it is applied, since later ops without
  it could build a state that never existed.  The system does not stop
  there:
  - **Corruption** (a length out of range, or a checksum / MAC failing
    under a key already proven good, or an unkeyed checksum): the WAL
    **heals**.  The bad entry and everything after it are copied, synced,
    into the quarantine as one record, the WAL is cut at the last good
    entry, and appends continue at once.  A `persistCtx` quarantines into
    `<basePath>.quarantine` (created only when needed); a bare `persist`
    uses `persistAttachQuarantine` / `persistAttachQuarantineFile`.
  - **A key problem** (no key resolves, or a keyed WAL whose first entry
    fails under a key not yet proven: it may only need the right key), **a
    store read error**, or corruption with no quarantine attached: the
    entry is **held**.  The WAL is left as found and appends return
    `PERSIST_LOG_REFUSED` until it reads (the right key bound, the store
    back) or `persistCompact` writes a fresh WAL.
  - Every new bad entry is reported once: `persistLastBadEntry` /
    `persistCtxLastBadEntry` give its kind (CORRUPT, KEY, READ), action
    (HEALED, HELD), offset, byte count and the entries before it, and
    `persistSetBadEntryNotify` / `persistCtxSetBadEntryNotify` call back
    (set it before `*OpenP` to hear about recovery).
  - A quarantine record is `"DKPQ"`, a version (uint16 LE, 1), 2 reserved
    bytes, the bad entry's WAL offset (int64 LE), the WAL's next sequence
    (uint64 LE), the byte count n (uint64 LE), then the n WAL bytes,
    exactly as they were.
- **Strict** (`strictRecovery = true`): an entry that fails to verify or
  apply fails the recovery, and the open returns NULL.

## Exact Values

Persistence stores values exactly as given: the string `"22"` stays a
string, a 128-bit integer stays 128 bits, a float stays a float.  The flexP
pushes use the exact write path, so what replay rebuilds is what was
written.  (See FLEX.md, "Two Write Paths".)

## Integrity and Keys

Snapshot bodies and WAL entries are each wrapped in an envelope with a
digest (`persistConfig.checksumType`):

- Checksums, no key: `PERSIST_CHECKSUM_XXH64`, `XXH3_64`, `XXH3_128`.  For
  local files on a trusted host.
- MACs, keyed: `PERSIST_CHECKSUM_HMAC_SHA256`, `HMAC_BLAKE3`, `BLAKE3_KEYED`
  (exactly 32-byte key).  For bytes an attacker could alter between writer
  and reader.  Set the key with `persistSetIntegrityKey`, or bind a keyring
  with `persistAttachKeyring(p, ring, keyId)`.  Writes are refused without a
  key.

A keyring binding re-checks the key on every operation.  After an
intentional rotation, call `persistRebindKeyring`; until then operations
fail with `PERSIST_ERR_KEY_GENERATION_MISMATCH`.  A file written under a
different key id fails verification with `PERSIST_ERR_KEY_ID_MISMATCH`, and
`persistLastExpectedKeyId` names the id the file wants.

Rotating to a new key id: the snapshot and the open WAL name the old id,
so after `persistRebindKeyring` to the new one appends are refused
(`PERSIST_ERR_KEY_ID_MISMATCH`) until `persistCompact` rewrites both under
the new key.  Rotate as rebind, compact, continue.  `persistSetIntegrityKey`
replaces every earlier key source (a keyring binding and a prepared key
included); set a prepared form of the new key after it.

A `persistCtx` holds one structure's files: the first `*NewP` / `*OpenP`
binds it to that structure's type, and every other type is refused on it.
Attaching a new snapshot or WAL store to a `persist` starts that store
fresh (entries still buffered go to the store they were logged to first).

## Formats and Versions

Each file header carries two version stamps: one for the file framing
(snapshot or WAL container) and one for the structure's own format (its
`persistOps` `formatVersion`, `minReadVersion` and `knownFeatures`).  A build
refuses a stamp it does not read, before any body byte is decoded, with
`PERSIST_ERR_CONTAINER_FORMAT` or `PERSIST_ERR_STRUCTURE_FORMAT`
(`persistLastFormat` gives the stamp it found).  A WAL this build refuses is
never appended to or overwritten.

`persistInspect(store, &info)` reads a header without the body: kind,
stamps, key id, element count and WAL sequence.  `persistVerifySnapshot` /
`persistVerifyWAL` check every envelope.

## Errors

A false return from the core API may leave a typed code in
`persistLastError(p)`:

| Code | Meaning |
|---|---|
| `PERSIST_ERR_KEY_GENERATION_MISMATCH` | the bound key was rotated since it was attached |
| `PERSIST_ERR_KEY_NOT_FOUND` | the bound key id is no longer in the keyring |
| `PERSIST_ERR_NO_KEY_ATTACHED` | a keyed algorithm with no key |
| `PERSIST_ERR_KEY_ID_MISMATCH` | the file was written under another key id |
| `PERSIST_ERR_CONTAINER_FORMAT` | the file framing's stamp is not readable here |
| `PERSIST_ERR_STRUCTURE_FORMAT` | the structure format's stamp is not readable here |

Other failures (I/O, a malformed file) return false and may leave
`PERSIST_ERR_NONE`: not every failure path sets a code.

Misuse of a context (a second wrapper, the wrong type) and an orphaned WAL
are also recorded as diagnostics on the calling thread
([DIAGNOSTICS.md](../core/DIAGNOSTICS.md)); persist never prints.

## Using the Core Directly

```c
/* map: a multimap of 2-element rows; row: as in the Quick Start */
persistConfig cfg = persistDefaultConfig();
persist *p = persistCreate(&persistOpsMultimap, &cfg);
persistAttachSnapshot(p, persistStoreFile("data/m.snap", true));
persistAttachWAL(p, persistStoreFile("data/m.wal", true));

persistSnapshot(p, map);                     /* starting point */
if (persistLogOp(p, PERSIST_OP_INSERT, row, 2) != PERSIST_LOG_REFUSED) {
    multimapInsert(&map, row);               /* apply after logging */
}

multimap *restored = persistRecover(p);      /* snapshot + WAL replay */
persistClose(p);
```

`persistStoreMemory` is an in-memory backend for tests.  A custom backend
fills in `persistStore`; providing `rewriteBegin`/`rewriteCommit`/
`rewriteAbort` gives it atomic snapshot publishing.

Built-in descriptors: `persistOpsFlex`, `persistOpsIntset`,
`persistOpsIntsetBig`, `persistOpsMultiroar`, `persistOpsHyperloglog`,
`persistOpsMultilist`, `persistOpsMultimap`, `persistOpsMultidict`,
`persistOpsMultiarray`, `persistOpsMultiOrderedSet`, `persistOpsMultiLRU`
(plus the keyring, cluster ring, mailbox watermark and AEAD nonce log
states), `persistOpsFenwick<T>` / `persistOpsFenwick2d<T>` for the typed
Fenwick trees (`fenwick/fenwickPersist.h`) and `persistOpsMultiFenwick`,
`persistOpsMultiFenwickFlat`, `persistOpsMultiFenwick2d` for the databox
ones (see FENWICK.md), `persistOpsSegment<T>` for the typed segment trees
(`segment/segmentPersist.h`; see SEGMENT.md), and `persistOpsMultimapAtom`,
`persistOpsMultimapAtomDense`, `persistOpsStringPool` and `persistOpsAtomPool`
for atom tables (`atomPersist.h`; see multimap/VARIANTS.md).

## Writing a persistOps Descriptor

A descriptor gives a type an id (`persistType`), a format stamp (a
non-zero `formatVersion`) and callbacks: `snapshot`/`restore`, `count`,
`estimateSize`, optionally `newEmpty`, `encodeOp`/`applyOp` for a WAL,
`validate` and `free`.

- `restore` receives bytes at any alignment: read multi-byte fields with
  `memcpy`.
- `restore` is where untrusted bytes enter: any truncation or corruption
  must give NULL or a valid structure, never a crash.
- `encodeOpInto` (optional) writes an op's body into memory the log gives
  it: the log then builds each entry with no body allocation.  Write the
  encoding once as `encodeOpInto` and make `encodeOp` from it with
  `persistOpEncodeViaInto`; `persistOpEncodeBoxesInto` (boxes as an exact
  flex) and `persistOpCopyInto` (fixed-width operands) are the bodies the
  built-in types use.  The intset, multiroar, multimap, multidict and
  multiOrderedSet descriptors set it.

Every built-in descriptor passes `persistContract` (`src/persist/contract/`)
at several sizes and seeds in every build configuration, and a new one
should be registered there too.  The contract checks:

1. snapshot, restore, snapshot gives identical bytes (the empty structure
   included);
2. operations encoded while building, replayed through `applyOp` onto the
   restored start, give the live snapshot's bytes;
3. every truncation and single-byte corruption restores to NULL or a valid
   structure;
4. strings that look like numbers keep their bytes through snapshot,
   restore and replay.
