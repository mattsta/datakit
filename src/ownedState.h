/* ownedState - single-writer publication and deferred reclamation
 *
 * Primitives for state that one writer (or one serialized set of
 * writers) publishes and other threads read without locking:
 *
 *   ownedArena  deferred-free list guarded by a reader counter. Readers
 *               bracket access with Enter/Exit; Retire parks an object
 *               and Drain releases it once no reader can still hold it.
 *   ownedCell   one published value: an atomic pointer whose displaced
 *               value is retired into an ownedArena (INDIRECT), or an
 *               atomic uint64_t (INLINE).
 *   ownedSlot   four-state occupancy word (EMPTY/ARMED/CLAIMED/RETIRED)
 *               that releases a slot's cargo exactly once when a retire
 *               races a claim.
 *   ownedSeq    ring of N seqlocked versions addressed by publication
 *               number; readers copy a version out and validate it.
 *
 * Threading contract
 *   - Publication is a release operation and reading is an acquire. A
 *     published value is never modified; a successor replaces it.
 *   - A declaration may supply a writer_ok predicate. A publication that
 *     fails it is counted and passed to note_violation, but is still
 *     performed: abandoning it partway would leave a half-written
 *     object. A NULL predicate disables the check.
 *   - Nothing here blocks waiting for readers. Drain samples the reader
 *     gate and leaves rows that are not yet safe for a later call.
 *   - No standalone atomic_thread_fence is used; ordering is carried by
 *     the atomic operations themselves, which ThreadSanitizer models.
 *
 * ownedArena release rules
 *   GRACE_AND_READERS (default): a row is released when graceUs has
 *   elapsed since it was retired AND the reader gate has been observed
 *   empty at some instant after the retire. The grace covers short
 *   readers that never enter the gate; the idle observation covers
 *   readers that do. A row still held after holdMultiple * graceUs is
 *   reported once through noteHold and kept, never freed.
 *
 *   READERS_ONLY: no clock is consulted. Valid only when every reader
 *   enters the gate before loading a retirable pointer. A row is
 *   released when the gate is sampled at zero under the arena lock, or
 *   when an exit that brought the gate to zero had already observed the
 *   row's retire epoch.
 *
 * Declarations (ownedArenaDecl, ownedCellDecl, ownedSlotDecl,
 * ownedSeqDecl) carry the caller's policy as function pointers and must
 * outlive every object that references them.
 */
#pragma once

#include "datakit.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "inflightGate.h"

/* Per-cargo callbacks. */

/* Release a retired cargo. ownedArena calls it from Drain and Destruct
 * with the arena lock held, so it must not block, take locks or call
 * back into the arena. ownedSlot calls it inline with no ownedState
 * lock held, and calls it even for a NULL cargo (the pointer is opaque
 * to the slot). */
typedef void (*ownedReleaseFn)(void *cargo);

/* Describe a cargo for hold reports: a static label (e.g. "entry") and
 * an id. Must accept NULL. */
typedef void (*ownedDescribeFn)(const void *cargo, const char **outLabel,
                                uint64_t *outId);

/* Report a row held past the arena's report horizon. Called once per
 * row, with the arena lock held. */
typedef void (*ownedHoldNoteFn)(void *ctx, const char *arenaName,
                                const char *label, uint64_t id, uint64_t ageUs,
                                uint32_t depth);

/* Monotonic clock in microseconds; injected so tests can drive time. */
typedef uint64_t (*ownedNowUsFn)(void);

/* What Retire does with a cargo it cannot park (row growth failed or the
 * arena is not initialised). */
typedef enum ownedOomPolicy {
    /* Release it now. Correct when readers reach the cargo only through
     * a container that has already unpublished it. */
    OWNED_OOM_FREE = 0,
    /* Leak it. Required when readers may hold the cargo pointer
     * directly, since releasing it could free it under a reader. */
    OWNED_OOM_LEAK = 1,
} ownedOomPolicy;

/* ---- ownedArena: reader gate and deferred release ---- */

/* One retired object. parkedUs is the retire time; heldNoted records
 * that the hold report fired; epoch is the retire sequence number
 * (READERS_ONLY only; 0 under the default rule). */
typedef struct ownedArenaRow {
    void *cargo;
    ownedReleaseFn release;
    ownedDescribeFn describe;
    uint64_t parkedUs;
    bool heldNoted;
    uint64_t epoch;
} ownedArenaRow;

/* How Drain decides a row is safe to release (see the file comment). */
typedef enum ownedReleaseRule {
    /* Grace elapsed and gate seen idle after the retire. */
    OWNED_RELEASE_GRACE_AND_READERS = 0,
    /* Gate seen at zero after the retire; no clock. */
    OWNED_RELEASE_READERS_ONLY = 1,
} ownedReleaseRule;

/* The arena's declaration; must outlive the arena. A zero-initialised
 * `release` and `reportHorizonUs` select the default rule. */
typedef struct ownedArenaDecl {
    const char *name;         /* used in hold reports */
    uint64_t graceUs;         /* GRACE_AND_READERS: minimum age */
    uint64_t holdMultiple;    /* GRACE_AND_READERS: report after
                                 holdMultiple * graceUs */
    ownedNowUsFn nowUs;       /* required */
    ownedHoldNoteFn noteHold; /* optional; NULL = count only */
    void *noteCtx;
    ownedReleaseRule release;
    uint64_t reportHorizonUs; /* READERS_ONLY: report rows older than
                                 this; 0 = never */
} ownedArenaDecl;

typedef struct ownedArena {
    /* Reader gate. idleUs is the largest clock value an exiting reader
     * read before observing the gate at zero, so idleUs > parkedUs means
     * the gate was empty at some instant after that row was retired. It
     * lets a gate that is never sampled idle still release rows. */
    inflightGate readers;
    _Atomic uint64_t idleUs;
    /* READERS_ONLY: retireEpoch increments on every retire (under lock);
     * idleEpoch is the largest retireEpoch an exiter read before a
     * decrement that brought the gate to zero. */
    _Atomic uint64_t retireEpoch;
    _Atomic uint64_t idleEpoch;

    /* Retirers and Drain both modify rows/count, so they hold `lock`.
     * The critical section grows (zrealloc) or compacts the array and
     * calls nowUs, noteHold and the rows' release and describe
     * callbacks, none of which may take locks. Callers may retire while
     * holding their own locks as long as this one is ordered last. */
    pthread_mutex_t lock;
    ownedArenaRow *rows;
    size_t count;
    size_t cap;

    const ownedArenaDecl *decl;
    bool initialized;
} ownedArena;

/* A declaration without nowUs leaves the arena uninitialised: Drain and
 * Destruct do nothing and Retire applies the OOM policy. */
void ownedArenaInit(ownedArena *arena, const ownedArenaDecl *decl);

/* Release every remaining row (held rows included), free the array and
 * destroy the lock. Call only after every thread that reads, retires or
 * drains has been joined. Idempotent. */
void ownedArenaDestruct(ownedArena *arena);

/* Reader scope. Enter before the first load that can return a
 * retirable pointer; Exit on every return path. Non-blocking: one atomic
 * RMW each. Under the default rule Exit also reads the clock before its
 * decrement and, when a load after the decrement reads zero, advances
 * idleUs with a CAS. Under READERS_ONLY it loads retireEpoch before the
 * decrement and, when that decrement reached zero, advances idleEpoch
 * the same way. NULL-safe. */
void ownedArenaEnter(ownedArena *arena);
void ownedArenaExit(ownedArena *arena);
uint32_t ownedArenaDepth(const ownedArena *arena);

/* Park cargo for deferred release. The cargo must already be
 * unreachable for new readers. A NULL cargo is ignored. */
void ownedArenaRetire(ownedArena *arena, void *cargo, ownedReleaseFn release,
                      ownedDescribeFn describe, ownedOomPolicy oom);

/* Release every row whose rule is satisfied. Never waits. */
void ownedArenaDrain(ownedArena *arena);

/* Process-wide counters across all arenas: sums of relaxed loads over
 * per-thread stripes, so updates from different threads do not share a
 * cache line. */
uint64_t ownedArenaDeferredTotal(void);  /* rows parked */
uint64_t ownedArenaRetiredTotal(void);   /* rows released */
uint64_t ownedArenaGraceHeldTotal(void); /* rows past grace with readers
                                            still present, per Drain */
uint64_t ownedArenaHeldReadersNeverLeftTotal(void); /* rows reported via
                                                       noteHold */
uint64_t ownedArenaLiveRows(void); /* parked, not yet released */

/* ---- ownedCell: single-writer publication ---- */

/* Descriptive only; not interpreted by ownedState. writer_ok is the
 * actual check. */
typedef enum ownedWriterDiscipline {
    OWNED_WRITER_CLASS = 0,      /* one designated thread */
    OWNED_WRITER_SERIALIZED = 1, /* any thread holding a given lock */
    OWNED_WRITER_CLAIM = 2,      /* exclusion by the datum's own CAS; no
                                    predicate */
    OWNED_WRITER_INSTANCE = 3,   /* a caller-defined set of threads */
} ownedWriterDiscipline;

typedef enum ownedCellForm {
    OWNED_CELL_INDIRECT = 0, /* _Atomic(void *): release exchange; the
                              * displaced value retires through an arena */
    OWNED_CELL_INLINE = 1,   /* _Atomic uint64_t: release store, no arena */
} ownedCellForm;

struct ownedCellDecl;

/* Return true if the calling thread is an allowed writer. owner is the
 * pointer given to Init. NULL disables the check. */
typedef bool (*ownedWriterOkFn)(const void *owner);

/* Report a publication that failed writer_ok. NULL = count only. */
typedef void (*ownedNoteViolationFn)(const struct ownedCellDecl *decl,
                                     const void *owner);

/* Shared by every cell of one type; per-instance data (owner, arena) is
 * passed to ownedCellInit. */
typedef struct ownedCellDecl {
    const char *name;
    ownedCellForm form;
    ownedWriterDiscipline discipline;
    ownedWriterOkFn writer_ok;
    ownedNoteViolationFn note_violation;
    /* INDIRECT only: how a displaced value is released. */
    ownedReleaseFn release;
    ownedDescribeFn describe;
    ownedOomPolicy oom;
} ownedCellDecl;

typedef struct ownedCell {
    union {
        _Atomic(void *) indirect;
        _Atomic uint64_t word;
    } v;
    const ownedCellDecl *decl;
    void *owner;
    /* INDIRECT only. NULL: a displaced value takes the declaration's
     * OOM policy at once. */
    ownedArena *arena;
} ownedCell;

/* `decl` must outlive the cell. `owner` is passed to writer_ok and
 * note_violation and is never dereferenced here. `arena` is ignored by
 * an INLINE cell. */
void ownedCellInit(ownedCell *c, const ownedCellDecl *decl, void *owner,
                   ownedArena *arena);

/* INDIRECT: run the writer check, publish `fresh` with a release
 * exchange and retire the displaced value. */
void ownedCellPublish(ownedCell *c, void *fresh);

/* INDIRECT: acquire load. Call only inside an Enter/Exit scope on the
 * cell's arena; the result stays valid until Exit. No writer check. */
const void *ownedCellRead(const ownedCell *c);

/* INLINE: writer check plus release store / acquire load. */
void ownedCellStore(ownedCell *c, uint64_t value);
uint64_t ownedCellLoad(const ownedCell *c);

/* Release the current value directly (not through the arena, which a
 * teardown has usually destructed already) and leave the cell empty.
 * INLINE cells zero their word. Idempotent. */
void ownedCellDestruct(ownedCell *c);

/* Process-wide count of publications that failed their declaration's
 * writer_ok predicate, for ownedCell and ownedSeq alike. */
uint64_t ownedCellWriterViolationTotal(void);

/* ---- ownedStateRow: per-name counters ---- */

/* A declaration may point at a static row. The row is enrolled into a
 * process-wide list on the primitive's first operation and never
 * removed, so ownedStateRowCount/ownedStateRowAt can enumerate every
 * declared name. Counters aggregate across all instances sharing a row,
 * and the counters are one shared cache line per row, so instances that
 * share a row contend for it under concurrent use.
 *
 * Enrolment is lock-free: a CAS on `enrolled` elects one thread, which
 * writes name and kind and pushes the row with a release CAS. Later
 * calls are a single acquire load. ownedSlot and ownedSeq enrol their
 * own rows; the ARENA and CELL kinds are for rows a caller enrols. */
typedef enum ownedStateKind {
    OWNED_ROW_ARENA = 0,
    OWNED_ROW_CELL,
    OWNED_ROW_SLOT,
    OWNED_ROW_SEQ,
} ownedStateKind;

#define OWNED_ROW_COUNTER_MAX 5 /* the widest kind's column count (SEQ) */

typedef struct ownedStateRow {
    const char *name;
    ownedStateKind kind;
    _Atomic uint64_t c[OWNED_ROW_COUNTER_MAX];
    struct ownedStateRow *next; /* registry link */
    _Atomic bool enrolled;
} ownedStateRow;

/* Idempotent and NULL-safe. Never blocks, so it may be called from any
 * context, including inside pthread_once. */
void ownedStateRowEnrol(ownedStateRow *row, const char *name,
                        ownedStateKind kind);
size_t ownedStateRowCount(void);
/* Rows are pushed at the head, so index 0 is the most recent enrolment.
 * An enrolment that runs concurrently with a Count/At walk shifts the
 * indices by one. O(i). */
const ownedStateRow *ownedStateRowAt(size_t i);
const char *ownedStateKindName(ownedStateKind kind);

/* ---- ownedSlot: claim/publish/retire over one in-place state ----
 *
 *   EMPTY    no cargo
 *   ARMED    cargo present, unclaimed
 *   CLAIMED  one thread owns the slot's fields exclusively
 *   RETIRED  terminal; no further claims
 *
 * Claim moves EMPTY/ARMED to CLAIMED; Publish moves CLAIMED back to
 * ARMED or EMPTY; Retire moves any other state to RETIRED. Cargo is
 * released once: by Retire if it displaced ARMED, or by the claimant's
 * Publish if Retire displaced CLAIMED (Publish's CAS then fails, and the
 * word records internally that the claimant's release is still owed, so
 * only that one Publish releases; the state still reads RETIRED).
 *
 * A zero-filled slot is valid (EMPTY == 0). With a NULL decl the slot
 * runs the same protocol but moves no counters and emits no notes.
 * Claim and Publish are one CAS each (Publish two when it releases),
 * Retire one CAS retried on interference; no allocation or locking. The
 * fields a claimant writes are published by the same word's release
 * operations, so no fence is needed. */
typedef enum ownedSlotState {
    OWNED_SLOT_EMPTY = 0,
    OWNED_SLOT_ARMED = 1,
    OWNED_SLOT_CLAIMED = 2,
    OWNED_SLOT_RETIRED = 3,
} ownedSlotState;

struct ownedSlotDecl;

/* Row columns, also passed to the note as the event. */
typedef enum ownedSlotEvent {
    OWNED_SLOT_C_RETIRE_MET_CLAIMED = 0,  /* a retire displaced CLAIMED */
    OWNED_SLOT_C_RELEASE_BY_CLAIMANT = 1, /* a publish-back met RETIRED and
                                           * the claimant released */
    OWNED_SLOT_C_CLAIM_REFUSED = 2,       /* a claim met CLAIMED or RETIRED */
} ownedSlotEvent;

/* Called on a refused claim, a retire that met CLAIMED, and a claimant
 * release. reading is the new value of the counter that moved (0 without
 * a row). NULL = count only. */
typedef void (*ownedSlotNoteFn)(void *ctx, const struct ownedSlotDecl *decl,
                                ownedSlotEvent event, ownedSlotState displaced,
                                uint64_t reading);

typedef struct ownedSlotDecl {
    const char *name;     /* the row's name */
    ownedSlotNoteFn note; /* NULL = count only */
    void *noteCtx;
    ownedStateRow *row; /* counters; NULL = none */
} ownedSlotDecl;

typedef struct ownedSlot {
    _Atomic int word;
    const ownedSlotDecl *decl;
} ownedSlot;

/* Static initialiser. */
#define OWNED_SLOT_INIT(declPtr) {.word = OWNED_SLOT_EMPTY, .decl = (declPtr)}
void ownedSlotInit(ownedSlot *s, const ownedSlotDecl *decl);

/* Acquire load of the state. */
ownedSlotState ownedSlotLoad(const ownedSlot *s);

/* EMPTY/ARMED -> CLAIMED (acq_rel). On success *out_prior says which
 * (ARMED means the fields hold cargo). Fails on CLAIMED or RETIRED,
 * counting claim_refused and notifying. */
bool ownedSlotClaim(ownedSlot *s, ownedSlotState *out_prior);

/* CLAIMED -> ARMED (still_armed) or EMPTY, with release. If a Retire
 * happened meanwhile, runs release(cargo) when release is non-NULL
 * (whatever the cargo), counts release_by_claimant, notifies, and
 * returns false; the slot stays RETIRED. Calling it without a live claim
 * is a misuse: on EMPTY or ARMED (never claimed), or on RETIRED (a claim
 * refused after a retire, or a claimant publishing again after its
 * release ran) it returns false, releases nothing and changes
 * nothing. */
bool ownedSlotPublish(ownedSlot *s, bool still_armed, ownedReleaseFn release,
                      void *cargo);

/* Move to RETIRED (acq_rel) and return the displaced state. Who
 * releases the cargo depends on that state:
 *
 *   EMPTY    nobody; there is no cargo
 *   ARMED    this call runs release(cargo)
 *   CLAIMED  the claimant, whose Publish then fails against RETIRED;
 *            this call counts retire_met_claimed and notifies
 *   RETIRED  nobody; an earlier Retire handled it */
ownedSlotState ownedSlotRetire(ownedSlot *s, ownedReleaseFn release,
                               void *cargo);

/* Per-declaration counters (0 when the decl has no row). */
uint64_t ownedSlotRetireMetClaimedTotal(const ownedSlotDecl *decl);
uint64_t ownedSlotReleaseByClaimantTotal(const ownedSlotDecl *decl);
uint64_t ownedSlotClaimRefusedTotal(const ownedSlotDecl *decl);

/* ---- ownedSeq: multi-version seqlock ring ----
 *
 * Publication n is written to version (n - 1) % N. Each version holds a
 * stamp word, a length word and the payload words. The stamp is 4n + 1
 * while publication n is being written and 4n once complete; 0 means
 * never written.
 *
 * Writers take a number with fetch_add on `assigned` and enter the
 * version with a CAS from a stamp that is not in progress and whose
 * number is below their own, to 4n + 1. They store the payload and exit
 * with a strong CAS to 4n. A writer that finds the version in progress,
 * or already holding a publication numbered at or above its own,
 * abandons without writing (counted as publish_abandoned). Its number
 * reads TORN while the version holds an older publication, and
 * OVERWRITTEN once it holds a newer one. `cursor` is a monotone max of
 * completed numbers. With one serialized writer it is the fully written
 * prefix; with concurrent writers (OWNED_WRITER_CLAIM) a number <=
 * cursor may still be in progress or abandoned.
 *
 * Readers copy the version and check the stamp before and after; they
 * never write to the version (a missed read moves a counter, below).
 * Every stamp, length and payload access on both sides is an atomic:
 * the writer's entry CAS acquires, its length and payload stores and its
 * exit CAS release, and every reader load acquires. The validation
 * argument in ownedState.c shows these orders suffice; no seq_cst total
 * order is needed. No accessor returns a pointer into a version.
 *
 * Versions are 8-byte aligned and packed at the stride below, so
 * adjacent versions share cache lines. A caller that wants one version
 * per line can pick version_bytes = line - 16 (48 for 64-byte lines,
 * 112 for 128-byte lines) and line-aligned storage.
 *
 * Lifecycle: the caller supplies 8-byte-aligned storage of
 * ownedSeqStorageBytes() (or OWNED_SEQ_STORAGE_WORDS for static storage)
 * zeroed before any other thread can reach the instance. Destruct only
 * after all readers and writers are joined. An instance with zero
 * versions or no storage ignores publishes, reads OVERWRITTEN, moves no
 * counter and enrols no row. Stamps wrap after 2^62 publications
 * (unsigned, so defined). */

typedef enum ownedSeqReadResult {
    OWNED_SEQ_VALID = 0,   /* dst holds all of publication `want` */
    OWNED_SEQ_TORN,        /* the version is being written or holds an
                            * older publication; a retry may succeed
                            * (see ownedSeqReadCanSucceed) */
    OWNED_SEQ_OVERWRITTEN, /* the version holds a later publication (or
                            * the instance has no versions); no retry can
                            * succeed */
    OWNED_SEQ_CAPACITY,    /* dst is smaller than the publication; nothing
                            * was copied and *outLen is the length needed */
} ownedSeqReadResult;

/* Per-declaration row columns. */
typedef enum ownedSeqColumn {
    OWNED_SEQ_C_PUBLISHED = 0,         /* exit CAS succeeded */
    OWNED_SEQ_C_TORN_READS = 1,        /* a read returned TORN */
    OWNED_SEQ_C_OVERWRITTEN_READS = 2, /* a read returned OVERWRITTEN */
    OWNED_SEQ_C_PUBLISH_ABANDONED = 3, /* entry found the version held or
                                        * newer; number consumed, nothing
                                        * written */
    OWNED_SEQ_C_REFUSED_OVERSIZE = 4,  /* len > version_bytes; nothing
                                        * written, no number assigned */
} ownedSeqColumn;

/* Opaque handle to the version a fill callback is writing. Writes go
 * through the two setters, which ignore out-of-range input. */
typedef struct ownedSeqVersionRef ownedSeqVersionRef;
void ownedSeqPutWord(ownedSeqVersionRef *v, size_t wordIndex, uint64_t w);
void ownedSeqPutBytes(ownedSeqVersionRef *v, size_t offset, const void *src,
                      size_t len);

typedef void (*ownedSeqFillFn)(ownedSeqVersionRef *v, void *ctx);

struct ownedSeqDecl;
/* Report a publication that failed writer_ok. NULL = count only. */
typedef void (*ownedSeqNoteViolationFn)(const struct ownedSeqDecl *decl,
                                        const void *owner);

typedef struct ownedSeqDecl {
    const char *name;     /* the row's name */
    uint32_t versions;    /* N >= 1. N == 1 is the classic seqlock. */
    size_t version_bytes; /* payload capacity of one version */
    ownedWriterDiscipline discipline; /* descriptive, as for ownedCell */
    ownedWriterOkFn writer_ok;        /* NULL disables the check */
    ownedSeqNoteViolationFn note_violation;
    ownedStateRow *row; /* counters; NULL = none */
} ownedSeqDecl;

typedef struct ownedSeq {
    _Atomic uint64_t assigned;
    _Atomic uint64_t cursor;
    void *versions; /* caller storage, N * stride bytes, 8-aligned */
    const ownedSeqDecl *decl;
    void *owner; /* passed to writer_ok; never dereferenced */
} ownedSeq;

/* Storage size for `decl`: N * stride, where the stride is
 * 16 + roundup8(version_bytes) (stamp word, length word, payload words).
 * The storage stays the caller's and is never freed here. A static
 * instance uses OWNED_SEQ_INIT over a zero-filled static block; a
 * runtime one uses ownedSeqInit, which zeroes the block. */
size_t ownedSeqStorageBytes(const ownedSeqDecl *decl);
/* The same size in words, as a constant expression for static storage:
 * `static _Atomic uint64_t storage[OWNED_SEQ_STORAGE_WORDS(N, B)]`. The
 * .c derives its stride from this macro. */
#define OWNED_SEQ_HEADER_WORDS 2u /* stamp, used */
#define OWNED_SEQ_STORAGE_WORDS(versions, versionBytes)                        \
    ((size_t)(versions) *                                                      \
     ((size_t)OWNED_SEQ_HEADER_WORDS + (((size_t)(versionBytes) + 7u) / 8u)))
#define OWNED_SEQ_INIT(declPtr, storagePtr)                                    \
    {.assigned = 0,                                                            \
     .cursor = 0,                                                              \
     .versions = (storagePtr),                                                 \
     .decl = (declPtr),                                                        \
     .owner = NULL}
void ownedSeqInit(ownedSeq *s, const ownedSeqDecl *decl, void *owner,
                  void *storage);
/* Clears the instance; frees nothing. Call only after every reader and
 * writer has been joined. */
void ownedSeqDestruct(ownedSeq *s);

/* Publish `len` bytes from `src` (Publish), or whatever `fill` writes
 * through the version handle (PublishWith). PublishWith's fill must
 * write every byte in [0, len): a reader that validates the publication
 * copies `len` bytes, and any byte the fill skipped holds whatever an
 * earlier publication left there. Never waits. The writer check runs
 * first and never refuses the write. Returns false, with no number
 * assigned, for a zero-version instance, for len > version_bytes
 * (counted as refused_oversize), and for len > 0 with nothing to write
 * it (Publish with a NULL src, PublishWith with a NULL fill). An
 * abandoned publication returns true and is counted as
 * publish_abandoned. */
bool ownedSeqPublish(ownedSeq *s, const void *src, size_t len);
bool ownedSeqPublishWith(ownedSeq *s, size_t len, ownedSeqFillFn fill,
                         void *ctx);

/* Copy publication `want` into `dst` and return the verdict. On VALID
 * and CAPACITY *outLen is the publication's length; otherwise 0. The
 * accessors are acquire loads. */
uint64_t ownedSeqCursor(const ownedSeq *s);   /* highest completed */
uint64_t ownedSeqAssigned(const ownedSeq *s); /* highest assigned */
uint32_t ownedSeqVersions(const ownedSeq *s); /* N */
ownedSeqReadResult ownedSeqReadAt(ownedSeq *s, uint64_t want, void *dst,
                                  size_t dstCap, size_t *outLen);

/* The stamp's low two bits. */
#define OWNED_SEQ_TAG_PUBLISHED 0u
#define OWNED_SEQ_TAG_INPROGRESS 1u

/* Out-of-line half of ownedSeqReadWordAt: enrols the row, counts the
 * verdict and returns it. Never returns VALID. */
ownedSeqReadResult ownedSeqReadMissed(ownedSeq *s, ownedSeqReadResult r);

/* Inline ownedSeqReadAt for declarations whose version_bytes is 8 and
 * whose writers always publish a full word. Same verdicts and counters
 * as ownedSeqReadAt; any other declaration reads OVERWRITTEN (counted
 * as an overwritten read). */
static inline ownedSeqReadResult ownedSeqReadWordAt(ownedSeq *s, uint64_t want,
                                                    uint64_t *out) {
    if (!s || !s->decl || !s->versions || s->decl->versions == 0) {
        return OWNED_SEQ_OVERWRITTEN; /* zero versions: no counter */
    }
    if (want == 0 || s->decl->version_bytes != sizeof(uint64_t)) {
        return ownedSeqReadMissed(s, OWNED_SEQ_OVERWRITTEN);
    }
    _Atomic uint64_t *const v =
        (_Atomic uint64_t *)((uint8_t *)s->versions +
                             (size_t)((want - 1) % s->decl->versions) *
                                 (OWNED_SEQ_STORAGE_WORDS(1u,
                                                          sizeof(uint64_t)) *
                                  sizeof(uint64_t)));
    const uint64_t published = 4u * want;
    const uint64_t v1 = atomic_load_explicit(&v[0], memory_order_acquire);
    if (v1 != published) {
        return ownedSeqReadMissed(
            s, (v1 == published + OWNED_SEQ_TAG_INPROGRESS || v1 < published)
                   ? OWNED_SEQ_TORN
                   : OWNED_SEQ_OVERWRITTEN);
    }
    const uint64_t w =
        atomic_load_explicit(&v[OWNED_SEQ_HEADER_WORDS], memory_order_acquire);
    const uint64_t v2 = atomic_load_explicit(&v[0], memory_order_acquire);
    if (v2 != published) {
        return ownedSeqReadMissed(s, OWNED_SEQ_OVERWRITTEN);
    }
    *out = w;
    return OWNED_SEQ_VALID;
}

/* True while fewer than N - 1 publications have been assigned since
 * assignedBefore (ownedSeqAssigned() taken when the reader began), i.e.
 * a retry may still find the wanted version. Always false for N < 2. */
bool ownedSeqReadCanSucceed(const ownedSeq *s, uint64_t assignedBefore);

/* Process-wide totals across all instances (relaxed loads). */
uint64_t ownedSeqPublishedTotal(void);
uint64_t ownedSeqTornReadsTotal(void);
uint64_t ownedSeqOverwrittenReadsTotal(void);
uint64_t ownedSeqPublishAbandonedTotal(void);
uint64_t ownedSeqPublishRefusedOversizeTotal(void);
/* One per-declaration column (0 when the decl has no row). */
uint64_t ownedSeqColumnTotal(const ownedSeqDecl *decl, ownedSeqColumn column);
