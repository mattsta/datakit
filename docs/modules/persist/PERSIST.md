# persist - Snapshots, Write-Ahead Log and Recovery

## Overview

`persist` makes datakit structures durable. A structure is saved as a
**snapshot** (its whole state) plus a **write-ahead log** (WAL) of the
operations made since that snapshot. **Compaction** folds the WAL into a
fresh snapshot, and **recovery** restores the snapshot and replays the WAL.

There are two layers:

- **P wrappers** (`flexP`, `intsetP`, `multimapP`, `multilistP`, `multidictP`,
  `multiOrderedSetP`, `multilruP`, `multiroarP`): a structure plus a
  `persistCtx`. Each mutator logs the operation, then applies it, so the
  files always describe the structure in memory. This is the layer most
  code wants.
- **The core** (`persist.h`): a `persist` handle bound to a `persistOps`
  descriptor (how one structure type snapshots, restores, encodes and applies
  operations) and to two `persistStore` backends (where the bytes go). Use
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

- **To one type.** The first `*NewP` / `*OpenP` binds the context to its
  structure type; any other type's New, Open, Recover, snapshot or compaction
  on it is refused.
- **To one live wrapper.** From a successful `*NewP` / `*OpenP` until its
  `*CloseP`, the wrapper holds the context; a second `*NewP` / `*OpenP` on it
  returns NULL. Close the first, then open again.

Both refusals report a misuse diagnostic (`DK_DIAG_PERSIST_CTX_WRONG_TYPE`,
`DK_DIAG_PERSIST_CTX_CLAIMED`; see [DIAGNOSTICS.md](../core/DIAGNOSTICS.md)).

`persistCtxProbe(basePath)` says what is on disk: `PERSIST_CTX_ABSENT`,
`PERSIST_CTX_PRESENT` (a snapshot an Open recovers) or
`PERSIST_CTX_ORPHAN_WAL` (a WAL without its snapshot, which every Open
treats as absent). `persistCtxExists` answers whether it is PRESENT and
reports an orphaned WAL as a `DK_DIAG_PERSIST_ORPHAN_WAL` condition.

### Every wrapper

Each wrapper has the same lifecycle and housekeeping calls, named after its
structure (`multimapNewP`, `multimapOpenP`, ...):

| Call                             | Does                                                                    |
| -------------------------------- | ----------------------------------------------------------------------- |
| `<s>NewP(ctx, ...)`              | a new structure; writes the first snapshot and holds the context        |
| `<s>OpenP(ctx)`                  | the structure recovered from snapshot + WAL (NULL: nothing to open)     |
| `<s>CloseP(w)`                   | syncs, frees the structure and lets the context go                      |
| `<s>SyncP(w)` / `<s>CompactP(w)` | sync the WAL now / fold it into a fresh snapshot now                    |
| `<s>GetStatsP(w, &stats)`        | the context's counters (ops, syncs, compactions, not-durable ops)       |
| `<s>CountP` / `<s>BytesP`        | as the structure's own Count / Bytes                                    |
| `<s>GetP(w)` / `<s>GetRawP(w)`   | the structure itself, for reads the wrapper does not mirror (read only) |

Writers log first and apply only when the log takes the op (see Mutation
Results); readers go straight to the structure. The writers and readers:

| Wrapper            | Writers (log, then apply)                                                                                                                                                                                                                    | Readers                                                                                                                                                                                                                                                                                                      |
| ------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `flexP`            | `flexPushHeadP`, `flexPushTailP`, `flexResetP`                                                                                                                                                                                               | `flexGetP`, `flexIndexP`, `flexGetByTypeP`                                                                                                                                                                                                                                                                   |
| `intsetP`          | `intsetAddP`, `intsetRemoveP`                                                                                                                                                                                                                | `intsetFindP`, `intsetGetAtP`, `intsetGetRawP`                                                                                                                                                                                                                                                               |
| `multimapP`        | `multimapInsertP`, `multimapInsertIfNotExistsP`, `multimapDeleteP`, `multimapDeleteFullWidthP`, `multimapFieldIncrP`, `multimapResetP`; `multimapNewConfigureP`                                                                              | `multimapLookupP`, `multimapExistsP`, `multimapFirstP`, `multimapLastP`, `multimapRandomValueP`, `multimapIteratorInitP`, `multimapIteratorInitAtP`, `multimapGetP`                                                                                                                                          |
| `multilistP`       | `multilistPushHeadP`, `multilistPushTailP`, `multilistPopHeadP`, `multilistPopTailP`, `multilistReplaceAtIndexP`, `multilistDelRangeP`, `multilistResetP`                                                                                    | `multilistIndexP`, `multilistIndexGetP`, `multilistIndexCheckP`, `multilistIteratorInitP`, `multilistIteratorInitForwardP`, `multilistIteratorInitReverseP`, `multilistIteratorInitForwardReadOnlyP`, `multilistIteratorInitReverseReadOnlyP`, `multilistGetP`                                               |
| `multidictP`       | `multidictAddP`, `multidictReplaceP`, `multidictDeleteP`, `multidictEmptyP`                                                                                                                                                                  | `multidictFindP`, `multidictExistsP`, `multidictIteratorInitP`, `multidictGetP`                                                                                                                                                                                                                              |
| `multiOrderedSetP` | `multiOrderedSetAddP`, `multiOrderedSetRemoveP`, `multiOrderedSetResetP`                                                                                                                                                                     | `multiOrderedSetGetScoreP`, `multiOrderedSetExistsP`, `multiOrderedSetGetByRankP`, `multiOrderedSetCountByScoreP`, `multiOrderedSetFirstP`, `multiOrderedSetLastP`, `multiOrderedSetIteratorInitP`, `multiOrderedSetIteratorInitAtScoreP`, `multiOrderedSetIteratorNextP`, `multiOrderedSetIteratorReleaseP` |
| `multilruP`        | `multilruInsertP`, `multilruInsertWeightedP`, `multilruIncreaseP`, `multilruUpdateWeightP`, `multilruDeleteP`, `multilruRemoveMinimumP`, `multilruSetMaxCountP`, `multilruSetMaxWeightP`; `multilruNewWithConfigP`, `multilruNewWithLevelsP` | `multilruGetLevelP`, `multilruGetWeightP`, `multilruTotalWeightP`, `multilruLevelCountP`, `multilruLevelWeightP`, `multilruIsPopulatedP`, `multilruGetMaxCountP`, `multilruGetMaxWeightP`                                                                                                                    |
| `multiroarP`       | `multiroarBitSetP`, `multiroarBitClearP`, `multiroarBitSetRangeP`, `multiroarBitClearRangeP`, `multiroarValueSetP`, `multiroarValueRemoveP`; `multiroarValueNewP`                                                                            | `multiroarBitGetP`, `multiroarBitCountP`, `multiroarRankP`, `multiroarSelectP`, `multiroarMinP`, `multiroarMaxP`, `multiroarIsEmptyP`, `multiroarValueGetP`, `multiroarValueCountP`, `multiroarMemoryUsageP`                                                                                                 |

A structure changed directly (through `GetRawP` or the structure's own
calls) is changed only in memory: the files no longer describe it, and the
next recovery loses the change. Write through the wrapper.

## Mutation Results

Every P mutator returns a `persistLogResult`:

| Result                    | Logged | Applied | Meaning                                                                                                 |
| ------------------------- | ------ | ------- | ------------------------------------------------------------------------------------------------------- |
| `PERSIST_LOG_REFUSED` (0) | no     | no      | encoding or the WAL write failed, or the structure refused the operation                                |
| `PERSIST_LOG_BUFFERED`    | yes    | yes     | in the WAL buffer; the sync policy writes it out later                                                  |
| `PERSIST_LOG_DURABLE`     | yes    | yes     | synced to stable storage                                                                                |
| `PERSIST_LOG_NOT_DURABLE` | yes    | yes     | the sync the policy asked for failed; durability unknown                                                |
| `PERSIST_LOG_UNCHANGED`   | no     | no      | the operation would change nothing (deleting an absent key, adding a present member, incrementing by 0) |

REFUSED is 0, so `if (result)` reads "the operation happened (or was a
no-op)". The rule for the core layer is the same: **apply the change exactly
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
  `multimapInsertIfNotExistsP`, `multiroarBitSetP`, `multiroarBitClearP`,
  `multiroarValueRemoveP`, `intsetAddP`, `intsetRemoveP`.

Durability is **best effort with notification**. A failed flush or sync
never undoes, blocks or aborts an operation: the result is NOT_DURABLE, the
failure is counted, and the next sync tries again. Watch the counters:

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

| Wrapper                                       | Out-parameters                                             |
| --------------------------------------------- | ---------------------------------------------------------- |
| `intsetAddP` / `intsetRemoveP`                | `bool *added` / `bool *removed`                            |
| `multiroarBitSetP` / `multiroarBitClearP`     | `bool *added` / `bool *cleared` (positions are `uint64_t`) |
| `multimapInsertP`                             | `bool *replaced`                                           |
| `multimapFieldIncrP`                          | `int64_t *newValue` (a refused increment is never logged)  |
| `multidictAddP`                               | `multidictResult *`                                        |
| `multiOrderedSetAddP`                         | `bool *updated`                                            |
| `multilruInsertP` / `multilruInsertWeightedP` | `multilruPtr *handle`                                      |

## Sync and Compaction

`persistCtxConfig` (from `persistCtxDefaultConfig()`):

- `syncMode`
  - `PERSIST_CTX_SYNC_NONE`: you call `persistCtxSync` (or the wrapper's
    `*SyncP`).
  - `PERSIST_CTX_SYNC_EVERYSEC`: at most one sync a second, triggered by
    operations. An operation followed by silence stays unsynced until the
    next operation, so call `persistCtxSyncIfDue` from your event loop to
    bound that window.
  - `PERSIST_CTX_SYNC_ALWAYS`: a sync after every operation.
- `compactAfterOps`, `compactAfterBytes`: compact when either is exceeded
  since the last compaction (`compactAfterBytes` measures the live WAL,
  its buffered bytes counted: `persistWALBytes`, and
  `persistCtxStats.bytesSinceCompact`)
  (0 disables that trigger). `*CompactP` compacts now.
- `walBufferSize`: the WAL write buffer.
- `strictRecovery`: see below.

A compaction writes a new snapshot that records the WAL sequence it includes,
then starts the WAL over. The file backend publishes the snapshot
atomically (it writes a temporary file, syncs it, renames it over the live
file and syncs the directory), so a crash leaves either the old snapshot or
the new one, never a torn file. If a crash lands between the new snapshot
and the WAL restart, recovery skips the WAL entries the snapshot already
includes, so nothing is applied twice.

## Recovery

`persistCtxOpen` + `*OpenP` restore the snapshot and replay the WAL.

- **Lenient** (default): an entry that verifies but does not apply is
  skipped and counted (`persistStats.entriesSkipped`). A torn tail (the
  last entry cut short by a crash) is truncated at the last complete
  entry. Replay stops at the first entry that does not read and keeps
  what came before; nothing after it is applied, since later ops without
  it could build a state that never existed. The system does not stop
  there:
  - **Corruption** (a length out of range, or a checksum / MAC failing
    under a key already proven good, or an unkeyed checksum): the WAL
    **heals**. The bad entry and everything after it are copied, synced,
    into the quarantine as one record, the WAL is cut at the last good
    entry, and appends continue at once. A `persistCtx` quarantines into
    `<basePath>.quarantine` (created only when needed); a bare `persist`
    uses `persistAttachQuarantine` / `persistAttachQuarantineFile`.
  - **A key problem** (no key resolves, or a keyed WAL whose first entry
    fails under a key not yet proven: it may only need the right key), **a
    store read error**, or corruption with no quarantine attached: the
    entry is **held**. The WAL is left as found and appends return
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
string, a 128-bit integer stays 128 bits, a float stays a float. The flexP
pushes use the exact write path, so what replay rebuilds is what was
written. (See FLEX.md, "Two Write Paths".)

## Integrity and Keys

Snapshot bodies and WAL entries are each wrapped in an envelope with a
digest (`persistConfig.checksumType`):

- Checksums, no key: `PERSIST_CHECKSUM_XXH64`, `XXH3_64`, `XXH3_128`. For
  local files on a trusted host.
- MACs, keyed: `PERSIST_CHECKSUM_HMAC_SHA256`, `HMAC_BLAKE3`, `BLAKE3_KEYED`
  (exactly 32-byte key). For bytes an attacker could alter between writer
  and reader. Set the key with `persistSetIntegrityKey`, or bind a keyring
  with `persistAttachKeyring(p, ring, keyId)`. Writes are refused without a
  key.

A keyring binding re-checks the key on every operation. After an
intentional rotation, call `persistRebindKeyring`; until then operations
fail with `PERSIST_ERR_KEY_GENERATION_MISMATCH`. A file written under a
different key id fails verification with `PERSIST_ERR_KEY_ID_MISMATCH`, and
`persistLastExpectedKeyId` names the id the file wants.

Rotating to a new key id: the snapshot and the open WAL name the old id,
so after `persistRebindKeyring` to the new one appends are refused
(`PERSIST_ERR_KEY_ID_MISMATCH`) until `persistCompact` rewrites both under
the new key. Rotate as rebind, compact, continue. `persistSetIntegrityKey`
replaces every earlier key source (a keyring binding and a prepared key
included); set a prepared form of the new key after it.

A `persistCtx` holds one structure's files: the first `*NewP` / `*OpenP`
binds it to that structure's type, and every other type is refused on it.
Attaching a new snapshot or WAL store to a `persist` starts that store
fresh (entries still buffered go to the store they were logged to first).

## Formats and Versions

Each file header carries two version stamps: one for the file framing
(snapshot or WAL container) and one for the structure's own format (its
`persistOps` `formatVersion`, `minReadVersion` and `knownFeatures`). A build
refuses a stamp it does not read, before any body byte is decoded, with
`PERSIST_ERR_CONTAINER_FORMAT` or `PERSIST_ERR_STRUCTURE_FORMAT`
(`persistLastFormat` gives the stamp it found). A WAL this build refuses is
never appended to or overwritten.

`persistInspect(store, &info)` reads a header without the body: kind,
stamps, key id, element count and WAL sequence. `persistVerifySnapshot` /
`persistVerifyWAL` check every envelope.

### Reading files by path (operators)

`persistOpenFiles(snapshotPath, walPath, &why)` opens a snapshot and a WAL
(either may be `NULL`) through read-only stores (`persistStoreFileReadOnly`:
the file is opened read-only, so nothing the context is asked to do can
change it) and picks the descriptor from the type the headers name
(`persistOpsForType`; `persistRegistry` lists every persisted type). The
result works with `persistInspectAttached`, `persistVerifySnapshot` /
`persistVerifyWAL`, `persistIterateWAL` and `persistRecover`; attach a
keyring first when a header names a key id. `persistClose` releases it. On
`NULL`, `why` names the check that refused: no path, a path that does not
open, a header that is not a persist header, a WAL given as the snapshot (or
the reverse), a type this build does not persist, or two files of different
types.

`persistIterateWAL(p, cb, ctx, &report)` hands each entry (sequence, offset
in the WAL, op, body bytes) to `cb`; `report` (optional) says why the walk
stopped: the end, the callback, a torn tail (the WAL ends inside an entry),
a bad entry (with its kind: CORRUPT, KEY, READ) or an unreadable header. It
also gives the end of the last complete entry (where a heal would cut), the
WAL's size and the count of entries read. `persistOpName` / `persistTypeName`
name ops and types; `persistOpenResultName`, `persistWALStopName`,
`persistBadEntryKindName`, `persistBadEntryActionName`,
`persistQuarantineReadName` and `persistErrName` give each status its stable
name, the enumerator's suffix (`NO_FILE`, `TORN`, `CORRUPT`, ...), and
`dkFormatResultName` does the same for format checks (`OK`, `WRONG_ID`, ...).
The Python package `datakit.persist` (`python/`) is these calls for
operators, one wheel for Python 3.15+. An op body's layout belongs to its type (some are
flexes, some fixed-width operands), so a generic reader shows the bytes.

`persistRepairFiles(snapshotPath, walPath, quarantinePath, &report, &why)`
repairs files IN PLACE as an open does (a torn tail cut, a corrupt entry
healed into a record at `quarantinePath`, a key or read problem held) and
reports the bad entry it met and what the files rebuild; operator tools run
it on copies (OPERATORS.md, "Repairing").

`persistRecoverSummarize(p, &summary)` recovers through the type's own
descriptor and reports what that rebuilt without keeping it: whether a
snapshot restored, whether the type's `validate` accepts the result, its
element count and estimated size. `persistGetStats` gives the entries
replayed and skipped. A torn or bad WAL entry ends the replay there; the
read-only stores never cut or heal it.

`persistQuarantineNext(store, &rec, &walBytes)` reads the quarantine record
at the store's position and leaves the store past it: `rec` gives the
record's own offset, the bad entry's WAL offset, the WAL's next sequence
and the byte count, and `walBytes` (optional; free with `zfree`) the bytes
themselves. It returns `PERSIST_QUARANTINE_RECORD`, `PERSIST_QUARANTINE_END`
at the end of the store, or `PERSIST_QUARANTINE_DAMAGED` for bytes that are
not a whole record (a short header, a wrong magic or version, a count past
the end of the store). The count is checked against the bytes left before
anything is allocated. Open a quarantine file with
`persistStoreFileReadOnly` and close it with `persistStoreClose` (which
closes and frees any store no persist owns).

## Errors

A false return from the core API may leave a typed code in
`persistLastError(p)`:

| Code                                  | Meaning                                           |
| ------------------------------------- | ------------------------------------------------- |
| `PERSIST_ERR_KEY_GENERATION_MISMATCH` | the bound key was rotated since it was attached   |
| `PERSIST_ERR_KEY_NOT_FOUND`           | the bound key id is no longer in the keyring      |
| `PERSIST_ERR_NO_KEY_ATTACHED`         | a keyed algorithm with no key                     |
| `PERSIST_ERR_KEY_ID_MISMATCH`         | the file was written under another key id         |
| `PERSIST_ERR_CONTAINER_FORMAT`        | the file framing's stamp is not readable here     |
| `PERSIST_ERR_STRUCTURE_FORMAT`        | the structure format's stamp is not readable here |

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

`persistStoreMemory` is an in-memory backend for tests
(`persistStoreMemoryBuffer` shows its bytes). A custom backend fills in
`persistStore`; providing `rewriteBegin`/`rewriteCommit`/`rewriteAbort`
gives it atomic snapshot publishing. A store no persist owns is closed with
`persistStoreClose`.

The core's calls by job:

| Job                     | Calls                                                                                                                                                                           |
| ----------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| make and bind           | `persistCreate`, `persistAttachSnapshot`, `persistAttachWAL`, `persistAttachQuarantine(File)`, `persistClose`                                                                   |
| configure               | `persistDefaultConfig`, `persistGetConfig`, `persistSetConfig`, `persistGetOps`                                                                                                 |
| keys                    | `persistSetIntegrityKey`, `persistSetIntegrityPreparedKey`, `persistAttachKeyring`, `persistRebindKeyring`                                                                      |
| write                   | `persistSnapshot`, `persistLogOp`, `persistSync`, `persistWALReady` (whether an append is taken now)                                                                            |
| compact                 | `persistShouldCompact`, `persistCompact`                                                                                                                                        |
| read back               | `persistRecover` (snapshot then WAL), or the steps: `persistRestore`, `persistReplayWAL`                                                                                        |
| check and inspect       | `persistVerifySnapshot`, `persistVerifyWAL`, `persistIterateWAL`, `persistInspect(Attached)`, `persistLastError`, `persistLastBadEntry`, `persistGetStats`, `persistResetStats` |
| the stores in use       | `persistSnapshotStore`, `persistWALStore`                                                                                                                                       |
| fault injection (tests) | `persistStoreFaulty`, `persistStoreFaultyArm`, `persistStoreFaultyDisarm`, `persistStoreFaultyCalls`                                                                            |

`persistSetConfig` replaces a live `persist`'s configuration for what it
does next. A new `walBufferSize` first writes out the buffered WAL bytes;
when that write fails the call returns false and keeps the old
configuration. A file already written keeps the digest it was written with:
every envelope records its own algorithm, and readers judge an entry by it.

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
for atom tables (`atomPersist.h`; see multimap/VARIANTS.md), and
`persistOpsIdRows` for rows addressed by id (`idRows.h`; format and ops in
`persist.h`).

### Applying ops outside recovery

`applyOp` for a multilist needs a working `mflexState` (its nodes may be
compressed). Recovery sets one up itself; code that calls a descriptor's
`applyOp` anywhere else (a follower applying a leader's ops, say) sets one
for the calling thread around the call, and clears it after:

```c
persistSetReplayState(state);   /* this thread only */
ops->applyOp(list, op, body, bodyLen); /* an encoded op, as the WAL holds it */
persistSetReplayState(NULL);
```

`persistGetReplayState()` returns the calling thread's setting.

A multilist POP in the WAL records the element it took, so a replay pops
that element rather than whatever is at the end. Two process-wide counters
report when that mattered: `persistMultilistPopIdentityDeepRemoveTotal()`
(the element was found away from the popping end, after interleaved
writes) and `persistMultilistPopIdentityAbsentNoopTotal()` (the element was
already gone, so the replayed pop changed nothing).

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
  it: the log then builds each entry with no body allocation. Write the
  encoding once as `encodeOpInto` and make `encodeOp` from it with
  `persistOpEncodeViaInto`; `persistOpEncodeBoxesInto` (boxes as an exact
  flex) and `persistOpCopyInto` (fixed-width operands) are the bodies the
  built-in types use. The intset, multiroar, multimap, multidict and
  multiOrderedSet descriptors set it.

Every built-in descriptor passes `persistContract` (`src/persist/contract/`)
at several sizes and seeds in every build configuration, and a new one
should be registered there too. The contract checks:

1. snapshot, restore, snapshot gives identical bytes (the empty structure
   included);
2. operations encoded while building, replayed through `applyOp` onto the
   restored start, give the live snapshot's bytes;
3. every truncation and single-byte corruption restores to NULL or a valid
   structure;
4. strings that look like numbers keep their bytes through snapshot,
   restore and replay;
5. a restored structure keeps its settings (size limits, compression,
   load factors and the like), with the builds varying them by seed.

The contract also checks that its type list and the registry
(`persistRegistry`, which `persistOpsForType` and the format manifest read)
name the same descriptors: a new descriptor goes in `persistRegistry.c` and
in the contract's list.

A new type's entry is a generator that builds the structure while it
records: `persistContractStart(log, structure)` right after creating it,
then `persistContractRecord(log, op, args, argc)` after each mutation, with
the arguments its `encodeOp` takes. `persistContractValue(&rng, &out, buf,
len)` draws a seeded test value (numbers of every width, true, false, null
and bytes, some of them digit strings), and `persistContractSameBytes(got,
want)` compares two bytes boxes by content, for a type whose values come
back as bytes. Atom descriptors share `atomPersistValueOk(value)`, the
values an atom may hold (a number, true, false, null or bytes).

Helpers for `encodeOp`: `persistOpNoBody` (an op with no operands, CLEAR or
an end pop), `persistOpEncodeBoxes` (the operand boxes as an exact flex),
`persistOpDecodeIndexValue` (an `[index, value]` body back into boxes);
`persistOpsFormat(ops)` is the format stamp a descriptor declares.

## Writing a P wrapper

A wrapper is a structure plus its `persistCtx`. It is small: one lifecycle
call per step, and one log call before each change.

```c
typedef struct counterP { persistCtx *ctx; intset *set; } counterP;

counterP *counterNewP(persistCtx *ctx) {
    intset *set = intsetNew();
    /* binds ctx to the type, writes the first snapshot, holds the ctx */
    if (!persistCtxStartNew(ctx, set, &persistOpsIntset)) {
        intsetFree(set);
        return NULL;
    }
    counterP *c = zmalloc(sizeof(*c));
    *c = (counterP){.ctx = ctx, .set = set};
    return c;
}

counterP *counterOpenP(persistCtx *ctx) {
    intset *set = persistCtxStartRecovered(ctx, &persistOpsIntset);
    if (!set) {
        return NULL; /* held elsewhere, another type, or nothing to recover */
    }
    counterP *c = zmalloc(sizeof(*c));
    *c = (counterP){.ctx = ctx, .set = set};
    return c;
}

persistLogResult counterAddP(counterP *c, int64_t v) {
    /* log first; apply exactly when the log took it */
    const persistLogResult r = persistCtxLogOp(c->ctx, PERSIST_OP_INSERT, &v, 1);
    if (r != PERSIST_LOG_REFUSED) {
        intsetAdd(&c->set, v, NULL);
        persistCtxMaybeCompact(c->ctx, c->set, &persistOpsIntset);
    }
    return r;
}

void counterCloseP(counterP *c) {
    persistCtxFinish(c->ctx); /* syncs, lets the context go */
    intsetFree(c->set);
    zfree(c);
}
```

- `persistCtxClaim` / `persistCtxRelease` are what StartNew / StartRecovered
  / Finish use to hold a context; a wrapper rarely calls them itself.
- A change persisted by a snapshot instead of a log entry (multilruP's
  limit changes are) asks `persistCtxCanLog` first, so it is refused
  exactly when a logged change would be, then writes the snapshot with
  `persistCtxCompact`. `persistCtxSaveSnapshot` writes a snapshot without
  resetting the WAL (StartNew's first snapshot).
- `persistCtxSync`, `persistCtxSyncIfDue` (for EVERYSEC from an event loop),
  `persistCtxCompact` and `persistCtxGetStats` back the wrapper's own Sync,
  Compact and GetStats calls; `persistCtxRecover` is recovery without
  holding the context, and `persistCtxInitForType` the binding step alone.
- Tests can make every log refuse with `persistCtxTestRefuseLogsBegin` /
  `End` (test builds), to check that a wrapper applies nothing it could not
  log.
