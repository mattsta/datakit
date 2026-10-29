# ownedState: single-writer publication and deferred reclamation

**Header**: `ownedState.h` **Source**: `ownedState.c`

ownedState is for state one writer (or one serialized set of writers) publishes and many threads read
without a lock. The writer replaces a value whole; readers load it with acquire semantics; the replaced
value is freed only once no reader can still hold it.

| Primitive    | Use it for                                                                                                                      |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------- |
| `ownedArena` | the reclamation half: readers bracket access with Enter/Exit, the writer Retires what it replaced, and Drain frees it when safe |
| `ownedCell`  | one published value: a pointer (INDIRECT, the replaced pointer retires through an arena) or a `uint64_t` (INLINE, no arena)     |
| `ownedSlot`  | a four-state slot (EMPTY / ARMED / CLAIMED / RETIRED) whose cargo is released exactly once when a retire races a claim          |
| `ownedSeq`   | a ring of N seqlocked versions addressed by publication number: readers copy a version out and check it was not overwritten     |

## The contract

- A published value is never modified. To change it, build a new one and publish it; the old one is
  retired.
- Readers of an INDIRECT cell or anything retirable call `ownedArenaEnter` before the first load and
  `ownedArenaExit` on every return path. What they read stays valid until their Exit.
- The writer retires only what new readers can no longer reach (it has already been replaced or
  unpublished).
- A cell's declaration may name a `writer_ok` predicate: a publication from a thread it rejects is
  counted (`ownedCellWriterViolationTotal`) and reported, which catches a second writer in tests.
- Declarations (`ownedArenaDecl`, `ownedCellDecl`, ...) are shared per type and must outlive every object
  that uses them; per-instance data goes to the Init calls.

## Example: a configuration many threads read

```c
#include "ownedState.h"
#include "timeUtil.h"

typedef struct config { uint32_t limit; char name[32]; } config;

static void configRelease(void *cargo) { zfree(cargo); }

static const ownedArenaDecl configArenaDecl = {
    .name = "config",
    .graceUs = 1000,           /* at least 1 ms after retire ... */
    .nowUs = timeUtilUs,       /* ... by this clock (required) */
};
static const ownedCellDecl configCellDecl = {
    .name = "config",
    .form = OWNED_CELL_INDIRECT,
    .discipline = OWNED_WRITER_CLASS,
    .release = configRelease,
};

static ownedArena arena;
static ownedCell current;

void configSetup(config *first) {
    ownedArenaInit(&arena, &configArenaDecl);
    ownedCellInit(&current, &configCellDecl, nullptr, &arena);
    ownedCellPublish(&current, first);
}

/* Any thread */
uint32_t configLimit(void) {
    ownedArenaEnter(&arena);
    const config *c = ownedCellRead(&current); /* valid until Exit */
    const uint32_t limit = c ? c->limit : 0;
    ownedArenaExit(&arena);
    return limit;
}

/* The writer: build, publish, then reclaim what is safe */
void configUpdate(uint32_t limit) {
    config *next = zmalloc(sizeof(*next));
    *next = (config){.limit = limit};
    ownedCellPublish(&current, next); /* the old one retires to 'arena' */
    ownedArenaDrain(&arena);          /* frees retired configs no reader holds */
}

void configTeardown(void) { /* after every reader thread is joined */
    ownedCellDestruct(&current);
    ownedArenaDestruct(&arena);
}
```

## Arenas without cells

A cell retires what it replaces by itself. Anything else readers can reach (a node unlinked from a
lock-free list, a table replaced by a resize) is retired directly once it is unreachable:

```c
ownedArenaRetire(&arena, oldTable, tableRelease, tableDescribe, OWNED_OOM_FREE);
ownedArenaDrain(&arena);
```

`describe` (may be NULL) labels a cargo in hold reports. The OOM policy says what happens when the arena
cannot record the row: `OWNED_OOM_FREE` releases at once (only safe when no reader can hold it),
`OWNED_OOM_LEAK` leaks it. `ownedArenaDepth` is the number of readers inside the arena right now, for tests
and diagnostics.

## Inline cells

An `OWNED_CELL_INLINE` cell publishes a `uint64_t` (a generation number, a packed small struct, a handle):
`ownedCellStore` is a release store (with the writer check) and `ownedCellLoad` an acquire load. Nothing
is retired, so no arena or reader scope is needed.

```c
static const ownedCellDecl epochDecl = {.name = "epoch", .form = OWNED_CELL_INLINE};
static ownedCell epoch;

ownedCellInit(&epoch, &epochDecl, nullptr, nullptr);
ownedCellStore(&epoch, 42);                 /* the writer */
const uint64_t now = ownedCellLoad(&epoch); /* any thread */
```

## Slots: one cargo, claimed or retired

An `ownedSlot` is one atomic state word beside fields the caller owns (a connection's buffers, a timer's
payload). A thread claims the slot to work on the fields; another may retire the slot at any moment (a
shutdown, a cancel). Whatever the interleaving, the cargo is released exactly once:

| Retire found | Who releases the cargo                                                     |
| ------------ | -------------------------------------------------------------------------- |
| `EMPTY`      | nobody (no cargo)                                                          |
| `ARMED`      | the retiring call                                                          |
| `CLAIMED`    | the claimant: its `ownedSlotPublish` fails against RETIRED and releases it |
| `RETIRED`    | nobody (an earlier retire did)                                             |

```c
static const ownedSlotDecl jobDecl = {.name = "job"};

typedef struct job {
    ownedSlot state;
    char *payload; /* set before the job is shared, never changed after */
} job;

void jobInit(job *j, char *payload) {
    ownedSlotInit(&j->state, &jobDecl);
    ownedSlotState prior;
    ownedSlotClaim(&j->state, &prior);           /* EMPTY -> CLAIMED */
    j->payload = payload;
    ownedSlotPublish(&j->state, true, nullptr, nullptr); /* -> ARMED */
}

/* Workers */
void jobRun(job *j) {
    ownedSlotState prior;
    if (!ownedSlotClaim(&j->state, &prior)) {
        return; /* another worker has it, or it was cancelled */
    }
    work(j->payload);
    /* Back to ARMED; false when a cancel came meanwhile, and then the
     * payload is released here, since the cancel could not */
    ownedSlotPublish(&j->state, true, zfree, j->payload);
}

/* Any thread */
void jobCancel(job *j) { ownedSlotRetire(&j->state, zfree, j->payload); }
```

`ownedSlotLoad` reads the state (acquire). A zero-filled slot is EMPTY, and `OWNED_SLOT_INIT(&decl)`
initialises a static one. With a row in the declaration, `ownedSlotClaimRefusedTotal`,
`ownedSlotRetireMetClaimedTotal` and `ownedSlotReleaseByClaimantTotal` count the races.

## Sequence rings: copy-out versions

An `ownedSeq` keeps the last N publications of a small fixed-size value in caller storage. A reader asks
for publication n and copies it out; the copy is checked against the version's stamp before and after, so
it is either whole or reported as lost. No pointer into a version is returned, so nothing needs
reclaiming. N = 1 is the classic seqlock; a larger N lets a slow reader still find a recent version.

```c
typedef struct quote { uint64_t bid, ask; } quote;
static const ownedSeqDecl quoteDecl = {
    .name = "quote", .versions = 4, .version_bytes = sizeof(quote)};
static _Atomic uint64_t quoteStorage[OWNED_SEQ_STORAGE_WORDS(4, sizeof(quote))];
static ownedSeq quotes = OWNED_SEQ_INIT(&quoteDecl, quoteStorage);

/* The writer */
ownedSeqPublish(&quotes, &(quote){.bid = 100, .ask = 101}, sizeof(quote));

/* A reader: the newest whole publication */
bool latestQuote(quote *out) {
    for (;;) {
        const uint64_t want = ownedSeqCursor(&quotes);
        if (want == 0) {
            return false; /* nothing published yet */
        }
        size_t len = 0;
        switch (ownedSeqReadAt(&quotes, want, out, sizeof(*out), &len)) {
        case OWNED_SEQ_VALID:
            return true;
        case OWNED_SEQ_CAPACITY:
            return false; /* 'out' is smaller than the publication */
        case OWNED_SEQ_TORN:
        case OWNED_SEQ_OVERWRITTEN:
            break; /* a newer publication landed: read the cursor again */
        }
    }
}
```

- A reader after one particular publication n retries TORN while `ownedSeqReadCanSucceed(&s, before)`
  (with `before = ownedSeqAssigned(&s)` taken when it began) says the version can still hold it.
- `ownedSeqInit(&s, &decl, owner, storage)` sets up a runtime instance over `ownedSeqStorageBytes(&decl)`
  bytes (it zeroes them); `ownedSeqDestruct` clears it once every reader and writer has stopped (the
  storage stays the caller's).
- `ownedSeqPublishWith(&s, len, fill, ctx)` writes in place through `ownedSeqPutWord` /
  `ownedSeqPutBytes` instead of copying from a buffer; the fill must write all `len` bytes.
- With `version_bytes` 8, `ownedSeqReadWordAt` reads one word inline; `ownedSeqReadMissed` is its
  out-of-line half, which counts the misses.
- `ownedSeqCursor` is the highest completed publication, `ownedSeqAssigned` the highest number handed out,
  `ownedSeqVersions` N.
- Counters: `ownedSeqPublishedTotal`, `ownedSeqTornReadsTotal`, `ownedSeqOverwrittenReadsTotal`,
  `ownedSeqPublishAbandonedTotal` and `ownedSeqPublishRefusedOversizeTotal` over the process;
  `ownedSeqColumnTotal(&decl, column)` for one declaration's row.

## The gate underneath: inflightGate

An arena's reader gate is an `inflightGate` (`inflightGate.h`), usable on its
own wherever a shutdown must wait for operations in flight:

```c
static inflightGate requests = INFLIGHT_GATE_INIT;
static _Atomic bool open = true;

bool handle(request *r) {
    inflightGateEnter(&requests);
    if (!atomic_load(&open)) {          /* the gate does not refuse work */
        inflightGateExit(&requests);
        return false;
    }
    serve(r);
    inflightGateExit(&requests);
    return true;
}

void shutdown(void) {
    atomic_store(&open, false);
    if (inflightGateAwaitZeroMs(&requests, 5000) == INFLIGHT_GATE_EXPIRED) {
        /* still busy after 5 s: the caller decides */
    }
}
```

- Enter is one atomic increment; Exit a load and a CAS, taking the gate's
  mutex only when it brings the count to zero while a waiter is parked
  (`inflightGateExitArmed`, out of line). `inflightGateExitPrev` returns the
  depth before its decrement (1: this exit reached zero).
- `inflightGateAwaitZeroMs(&g, ms)` parks until the count is zero (IDLE) or
  the budget ends (EXPIRED); `inflightGateVerdictName` names a verdict. After
  IDLE the gate may be freed, provided nothing can Enter it again.
- `inflightGateDepth` is a relaxed read for diagnostics;
  `inflightGateSampleLatest` reads the latest count with an atomic RMW.
  `inflightGateWakeAllWaiters` wakes parked waiters to re-check (harmless when
  the gate is busy).
- All scopes share one count word, so many cores entering at once contend
  on its cache line.

## Release rules

- `OWNED_RELEASE_GRACE_AND_READERS` (the default): a retired object is freed once `graceUs` has passed
  since its retire AND the reader gate has been seen empty at some instant after it. Rows still held after
  `holdMultiple * graceUs` are reported once (`noteHold`) and kept, never freed under a reader.
- `OWNED_RELEASE_READERS_ONLY`: no clock decides; valid only when every reader enters the gate before
  loading a retirable pointer.

`ownedArenaDrain` never waits; call it from the writer's loop. Counters for monitoring:
`ownedArenaDeferredTotal`, `ownedArenaRetiredTotal`, `ownedArenaLiveRows`, `ownedArenaGraceHeldTotal`,
`ownedArenaHeldReadersNeverLeftTotal`.

Per-name counters: a declaration that points at a static `ownedStateRow` enrols it on first use
(`ownedStateRowEnrol` enrols one by hand). `ownedStateRowCount` and `ownedStateRowAt(i)` walk every
enrolled row (newest first) and `ownedStateKindName` names a row's kind, for a metrics exporter:

```c
for (size_t i = 0; i < ownedStateRowCount(); i++) {
    const ownedStateRow *row = ownedStateRowAt(i);
    printf("%s %s %" PRIu64 "\n", ownedStateKindName(row->kind), row->name,
           atomic_load_explicit(&row->c[0], memory_order_relaxed));
}
```

## Costs and cautions

- Every Enter and Exit is one atomic read-modify-write on the gate's count word: readers on many cores
  entering at once contend on that cache line, as the read side of a reader-writer lock does. Neither rule
  reads the clock on the reader path.
- A release callback runs with the arena's lock held: it must not call back into the arena or wait on a
  lock a retiring thread may hold. An allocator's free is fine.
- `ownedSeq` (versions of a fixed byte size) suits values small enough to copy on every read; a reader
  that loses a race gets `OWNED_SEQ_OVERWRITTEN` or `OWNED_SEQ_TORN` and retries.
- `ownedSlot` is for one cargo whose owner may change hands (claim / publish / retire); the claimant or the
  retirer, never both, releases it.
