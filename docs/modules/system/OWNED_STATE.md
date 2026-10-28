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

## Release rules

- `OWNED_RELEASE_GRACE_AND_READERS` (the default): a retired object is freed once `graceUs` has passed
  since its retire AND the reader gate has been seen empty at some instant after it. Rows still held after
  `holdMultiple * graceUs` are reported once (`noteHold`) and kept, never freed under a reader.
- `OWNED_RELEASE_READERS_ONLY`: no clock decides; valid only when every reader enters the gate before
  loading a retirable pointer.

`ownedArenaDrain` never waits; call it from the writer's loop. Counters for monitoring:
`ownedArenaDeferredTotal`, `ownedArenaRetiredTotal`, `ownedArenaLiveRows`, `ownedArenaGraceHeldTotal`,
`ownedArenaHeldReadersNeverLeftTotal`; per-name rows via `ownedStateRowEnrol` / `ownedStateRowAt`.

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
