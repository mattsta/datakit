/* ownedState - see ownedState.h for the primitives and threading
 * contract. The ownedSeq ordering argument is above its implementation
 * below. */

#include "ownedState.h"
#include "dkDiag.h"

#include <string.h>

/* Process-wide counters (relaxed), striped. Unrelated instances updated
 * by different threads would otherwise move one shared cache line on
 * every publish and every missed read. Each thread takes a stripe, round
 * robin, on its first update; a total is the sum over the stripes. A
 * stripe on its own may wrap (live rows are added on one thread and
 * removed on another), but the sum modulo 2^64 is exact. A total read
 * while updates are in flight is a sum of relaxed loads and may miss
 * some of them, as a relaxed load of a single counter may. */
typedef enum ownedTotal_ {
    OWNED_T_ARENA_DEFERRED = 0,
    OWNED_T_ARENA_RETIRED,
    OWNED_T_ARENA_GRACE_HELD,
    OWNED_T_ARENA_NEVER_LEFT,
    OWNED_T_ARENA_LIVE_ROWS,
    OWNED_T_WRITER_VIOLATION,
    OWNED_T_SEQ_PUBLISHED,
    OWNED_T_SEQ_TORN_READS,
    OWNED_T_SEQ_OVERWRITTEN_READS,
    OWNED_T_SEQ_PUBLISH_ABANDONED,
    OWNED_T_SEQ_REFUSED_OVERSIZE,
    OWNED_T_COUNT_
} ownedTotal_;

#define OWNED_STRIPES 16u
/* Covers the 128-byte lines of Apple silicon and the adjacent-line
 * prefetch pairs of x86-64. */
#define OWNED_STRIPE_ALIGN 128

typedef struct ownedStripe_ {
    _Alignas(OWNED_STRIPE_ALIGN) _Atomic uint64_t c[OWNED_T_COUNT_];
} ownedStripe_;

static ownedStripe_ g_owned_stripes[OWNED_STRIPES];
static _Atomic uint32_t g_owned_stripe_next = 0;
static _Thread_local ownedStripe_ *g_owned_stripe_mine = NULL;

static ownedStripe_ *ownedStripeMine_(void) {
    ownedStripe_ *s = g_owned_stripe_mine;
    if (!s) {
        /* @sync: relaxed counter */
        const uint32_t i = atomic_fetch_add_explicit(&g_owned_stripe_next, 1,
                                                     memory_order_relaxed);
        s = &g_owned_stripes[i % OWNED_STRIPES];
        g_owned_stripe_mine = s;
    }
    return s;
}

static void ownedTotalAdd_(ownedTotal_ t) {
    /* @sync: relaxed counter */
    atomic_fetch_add_explicit(&ownedStripeMine_()->c[t], 1,
                              memory_order_relaxed);
}

static void ownedTotalSub_(ownedTotal_ t) {
    /* @sync: relaxed counter */
    atomic_fetch_sub_explicit(&ownedStripeMine_()->c[t], 1,
                              memory_order_relaxed);
}

static uint64_t ownedTotalRead_(ownedTotal_ t) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < OWNED_STRIPES; i++) {
        /* @sync: relaxed counter */
        sum += atomic_load_explicit(&g_owned_stripes[i].c[t],
                                    memory_order_relaxed);
    }
    return sum;
}

uint64_t ownedArenaDeferredTotal(void) {
    return ownedTotalRead_(OWNED_T_ARENA_DEFERRED);
}

uint64_t ownedArenaRetiredTotal(void) {
    return ownedTotalRead_(OWNED_T_ARENA_RETIRED);
}

uint64_t ownedArenaGraceHeldTotal(void) {
    return ownedTotalRead_(OWNED_T_ARENA_GRACE_HELD);
}

uint64_t ownedArenaHeldReadersNeverLeftTotal(void) {
    return ownedTotalRead_(OWNED_T_ARENA_NEVER_LEFT);
}

uint64_t ownedArenaLiveRows(void) {
    return ownedTotalRead_(OWNED_T_ARENA_LIVE_ROWS);
}

uint64_t ownedCellWriterViolationTotal(void) {
    return ownedTotalRead_(OWNED_T_WRITER_VIOLATION);
}

/* ====================================================================
 * ownedArena
 * ==================================================================== */

void ownedArenaInit(ownedArena *arena, const ownedArenaDecl *decl) {
    if (!arena) {
        return;
    }
    arena->rows = NULL;
    arena->count = 0;
    arena->cap = 0;
    arena->decl = decl;
    arena->initialized = false;
    arena->readers = (inflightGate)INFLIGHT_GATE_INIT;
    /* @sync: relaxed init */
    atomic_store_explicit(&arena->idleUs, 0, memory_order_relaxed);
    /* @sync: relaxed init */
    atomic_store_explicit(&arena->retireEpoch, 0, memory_order_relaxed);
    /* @sync: relaxed init */
    atomic_store_explicit(&arena->idleEpoch, 0, memory_order_relaxed);
    /* Without a clock no row can be judged; leave the arena
     * uninitialised. */
    if (!decl || !decl->nowUs) {
        return;
    }
    /* A calloc zero-fill is NOT a portable pthread_mutex_t init, and an
     * owner embedded by value reaches no static initialiser. */
    pthread_mutex_init(&arena->lock, NULL);
    arena->initialized = true;
}

void ownedArenaDestruct(ownedArena *arena) {
    if (!arena || !arena->initialized) {
        return;
    }
    /* Runs after all users are joined: release every row
     * unconditionally, without the lock. */
    for (size_t i = 0; i < arena->count; i++) {
        if (arena->rows[i].release) {
            arena->rows[i].release(arena->rows[i].cargo);
        }
        ownedTotalAdd_(OWNED_T_ARENA_RETIRED);
        ownedTotalSub_(OWNED_T_ARENA_LIVE_ROWS);
    }
    zfree(arena->rows);
    arena->rows = NULL;
    arena->count = 0;
    arena->cap = 0;
    pthread_mutex_destroy(&arena->lock);
    /* Cleared with the lock it describes, which also makes this
     * idempotent. */
    arena->initialized = false;
}

void ownedArenaEnter(ownedArena *arena) {
    if (!arena) {
        return;
    }
    inflightGateEnter(&arena->readers);
}

/* Both rules: read retireEpoch before the decrement; if this decrement
 * reached zero, publish that epoch to idleEpoch (monotone max, so
 * concurrent exiters never move it back). The acquire load reads from
 * the retire that stamped the epoch (or a later one), so that retire,
 * and the unpublish before it, happen before this exit. No clock is
 * read. */
void ownedArenaExit(ownedArena *arena) {
    if (!arena) {
        return;
    }
    /* @sync: acquire arena.epoch */
    const uint64_t e =
        atomic_load_explicit(&arena->retireEpoch, memory_order_acquire);
    if (inflightGateExitPrev(&arena->readers) != 1) {
        return;
    }
    /* @sync: relaxed seed */
    uint64_t prev =
        atomic_load_explicit(&arena->idleEpoch, memory_order_relaxed);
    /* @sync: release arena.idle */
    while (prev < e && !atomic_compare_exchange_weak_explicit(
                           &arena->idleEpoch, &prev, e, memory_order_release,
                           memory_order_relaxed)) {
        /* prev reloaded by the exchange. */
    }
}

uint32_t ownedArenaDepth(const ownedArena *arena) {
    if (!arena) {
        return 0;
    }
    return inflightGateDepth(&((ownedArena *)arena)->readers);
}

void ownedArenaRetire(ownedArena *arena, void *cargo, ownedReleaseFn release,
                      ownedDescribeFn describe, ownedOomPolicy oom) {
    if (!cargo) {
        return;
    }
    if (!arena || !arena->initialized) {
        if (oom == OWNED_OOM_FREE && release) {
            release(cargo);
        }
        return;
    }
    /* Retire and Drain both modify the row array. */
    pthread_mutex_lock(&arena->lock);
    if (arena->count == arena->cap) {
        const size_t newcap = arena->cap ? arena->cap * 2 : 8;
        /* @own: container; free=ownedArenaDestruct */
        ownedArenaRow *rows = zrealloc(arena->rows, newcap * sizeof(*rows));
        if (!rows) {
            /* Not counted: the row never entered the arena. */
            pthread_mutex_unlock(&arena->lock);
            if (oom == OWNED_OOM_FREE && release) {
                release(cargo);
            }
            return;
        }
        arena->rows = rows;
        arena->cap = newcap;
    }
    const size_t i = arena->count;
    /* Epochs are assigned under the lock, so rows carry increasing
     * epochs; the release pairs with the exiter's acquire. Both rules use
     * them. */
    /* @sync: release arena.epoch */
    const uint64_t epoch = atomic_fetch_add_explicit(&arena->retireEpoch, 1,
                                                     memory_order_release) +
                           1;
    arena->rows[i] = (ownedArenaRow){.cargo = cargo,
                                     .release = release,
                                     .describe = describe,
                                     .parkedUs = arena->decl->nowUs(),
                                     .heldNoted = false,
                                     .epoch = epoch};
    arena->count = i + 1;
    pthread_mutex_unlock(&arena->lock);
    ownedTotalAdd_(OWNED_T_ARENA_DEFERRED);
    ownedTotalAdd_(OWNED_T_ARENA_LIVE_ROWS);
}

/* GRACE_AND_READERS report horizon, holdMultiple * graceUs, saturating:
 * a product past UINT64_MAX is a horizon no age exceeds (never report),
 * not a wrapped and early one. A zero product reports a row as soon as
 * it is past grace with readers present. */
static uint64_t ownedArenaGraceHorizonUs_(const ownedArenaDecl *decl) {
    if (decl->graceUs != 0 && decl->holdMultiple > UINT64_MAX / decl->graceUs) {
        return UINT64_MAX;
    }
    return decl->holdMultiple * decl->graceUs;
}

/* Every release callback runs here, under the arena lock, so a Retire on
 * another thread waits for the whole batch. */
void ownedArenaDrain(ownedArena *arena) {
    if (!arena || !arena->initialized) {
        return;
    }
    /* Check emptiness under the lock; a retirer may be appending. */
    pthread_mutex_lock(&arena->lock);
    if (arena->count == 0) {
        pthread_mutex_unlock(&arena->lock);
        return;
    }
    /* Sample the gate once, under the lock, so every row present was
     * retired before the sample.  Why a zero sample makes a row safe
     * (both rules), for a reader R that could hold the row's cargo O:
     *
     *   The sample is an RMW (add 0), so it reads the LAST value in the
     *   count's modification order, and R's Enter (also an RMW) is either
     *   before or after it in that order.
     *   - Before: the sample's value includes R's increment unless R's
     *     Exit is also before it; a zero therefore means R has exited,
     *     and R reads O only inside its scope.
     *   - After: R's Enter reads from the sample or a later RMW, all in
     *     the release sequence headed by the seq_cst sample, so the
     *     sample synchronizes with R's Enter.  The unpublish of O (the
     *     cell exchange) and its retire are sequenced before this Drain's
     *     lock, hence before the sample; so they happen before R's cell
     *     load, which by coherence reads O's successor or later, never O.
     *   A plain acquire load would not do: it may read an older count, and
     *   "reader increments then loads the cell / writer swaps the cell then
     *   loads the count" is the store-buffering pattern C11 permits to miss
     *   on both sides.  The grace period proves nothing here: it is a
     *   timing allowance, the only one a reader that skips the gate gets.
     *
     * Both rules also accept the epoch: an exiter read retireEpoch >=
     * row.epoch (acquire) before a decrement that reached zero, so the
     * gate was empty at an instant after the row's retire, and every
     * scope entered before that instant has exited; a scope entered after
     * it reads from that decrement's release sequence and sees the
     * unpublish, as above.  idleEpoch carries this here by
     * release/acquire.  Only the default rule's grace and both rules'
     * hold reports read the clock. */
    const bool readers_only =
        arena->decl->release == OWNED_RELEASE_READERS_ONLY;
    const bool gate_idle_now = inflightGateSampleLatest(&arena->readers) == 0;
    /* @sync: acquire arena.idle */
    const uint64_t idle_epoch =
        atomic_load_explicit(&arena->idleEpoch, memory_order_acquire);
    const uint64_t now = arena->decl->nowUs();
    size_t w = 0;
    for (size_t r = 0; r < arena->count; r++) {
        ownedArenaRow row = arena->rows[r];
        /* Monotonic clock: now >= parkedUs, no underflow. */
        const uint64_t age = now - row.parkedUs;
        const bool c1 = readers_only || age > arena->decl->graceUs;
        const bool c2 = gate_idle_now || idle_epoch >= row.epoch;
        if (c1 && c2) {
            if (row.release) {
                row.release(row.cargo);
            }
            ownedTotalAdd_(OWNED_T_ARENA_RETIRED);
            ownedTotalSub_(OWNED_T_ARENA_LIVE_ROWS);
            continue;
        }
        if (c1) {
            /* Past grace but readers remain. */
            ownedTotalAdd_(OWNED_T_ARENA_GRACE_HELD);
            /* Report horizon. */
            const bool past_horizon =
                readers_only ? (arena->decl->reportHorizonUs > 0 &&
                                age > arena->decl->reportHorizonUs)
                             : (age > ownedArenaGraceHorizonUs_(arena->decl));
            if (!row.heldNoted && past_horizon) {
                /* Report once per row; the row is kept. */
                row.heldNoted = true;
                ownedTotalAdd_(OWNED_T_ARENA_NEVER_LEFT);
                if (arena->decl->noteHold) {
                    const char *label = "cargo";
                    uint64_t id = 0;
                    if (row.describe) {
                        row.describe(row.cargo, &label, &id);
                    }
                    arena->decl->noteHold(arena->decl->noteCtx,
                                          arena->decl->name, label, id, age,
                                          inflightGateDepth(&arena->readers));
                }
            }
        }
        /* Survivor: compact toward the front, keep retire order. */
        arena->rows[w] = row;
        w++;
    }
    arena->count = w;
    pthread_mutex_unlock(&arena->lock);
}

/* ====================================================================
 * ownedCell
 * ==================================================================== */

/* Count and report a publication by a disallowed writer; never refuse
 * it. NULL predicate = no check. */
static void ownedCellWriterCheck_(const ownedCell *c) {
    const ownedCellDecl *const d = c->decl;
    if (!d->writer_ok) {
        return;
    }
    if (d->writer_ok(c->owner)) {
        return;
    }
    ownedTotalAdd_(OWNED_T_WRITER_VIOLATION);
    DK_DIAG(DK_DIAG_OWNED_WRITER_VIOLATION,
            "cell %s published by a writer its declaration does not allow",
            d->name ? d->name : "(unnamed)");
    if (d->note_violation) {
        d->note_violation(d, c->owner);
    }
}

void ownedCellInit(ownedCell *c, const ownedCellDecl *decl, void *owner,
                   ownedArena *arena) {
    if (!c) {
        return;
    }
    c->decl = decl;
    c->owner = owner;
    c->arena = arena;
    if (decl && decl->form == OWNED_CELL_INLINE) {
        /* @sync: relaxed init */
        atomic_store_explicit(&c->v.word, 0, memory_order_relaxed);
    } else {
        /* @sync: relaxed init */
        atomic_store_explicit(&c->v.indirect, NULL, memory_order_relaxed);
    }
}

void ownedCellPublish(ownedCell *c, void *fresh) {
    if (!c || !c->decl || c->decl->form != OWNED_CELL_INDIRECT) {
        return;
    }
    ownedCellWriterCheck_(c);
    /* acq_rel: the release half publishes `fresh`; the acquire half lets
     * this writer, which now owns `stale` and may release it inline, see
     * the initialisation by whichever writer published it, even when no
     * other happens-before edge orders the two writers. */
    /* @sync: acq_rel cell.value */
    void *const stale =
        atomic_exchange_explicit(&c->v.indirect, fresh, memory_order_acq_rel);
    if (!stale) {
        return;
    }
    if (c->arena) {
        ownedArenaRetire(c->arena, stale, c->decl->release, c->decl->describe,
                         c->decl->oom);
        return;
    }
    /* No arena: apply the OOM policy directly. */
    if (c->decl->oom == OWNED_OOM_FREE && c->decl->release) {
        c->decl->release(stale);
    }
}

const void *ownedCellRead(const ownedCell *c) {
    if (!c || !c->decl || c->decl->form != OWNED_CELL_INDIRECT) {
        return NULL;
    }
    /* @sync: acquire cell.value */
    return atomic_load_explicit(&((ownedCell *)c)->v.indirect,
                                memory_order_acquire);
}

void ownedCellStore(ownedCell *c, uint64_t value) {
    if (!c || !c->decl || c->decl->form != OWNED_CELL_INLINE) {
        return;
    }
    ownedCellWriterCheck_(c);
    /* @sync: release cell.word */
    atomic_store_explicit(&c->v.word, value, memory_order_release);
}

uint64_t ownedCellLoad(const ownedCell *c) {
    if (!c || !c->decl || c->decl->form != OWNED_CELL_INLINE) {
        return 0;
    }
    /* @sync: acquire cell.word */
    return atomic_load_explicit(&((ownedCell *)c)->v.word,
                                memory_order_acquire);
}

void ownedCellDestruct(ownedCell *c) {
    if (!c || !c->decl) {
        return;
    }
    if (c->decl->form == OWNED_CELL_INLINE) {
        /* @sync: relaxed init */
        atomic_store_explicit(&c->v.word, 0, memory_order_relaxed);
        return;
    }
    /* Release the current value directly; the arena is usually already
     * destructed during teardown, and no reader remains. */
    /* @sync: relaxed init */
    void *const cur =
        atomic_exchange_explicit(&c->v.indirect, NULL, memory_order_relaxed);
    if (cur && c->decl->release) {
        c->decl->release(cur);
    }
}

/* ====================================================================
 * ownedStateRow
 * ==================================================================== */

/* Registry of enrolled rows: a Treiber list pushed at the head and never
 * popped, so enumeration needs no lock and enrolment never blocks. */
static _Atomic(ownedStateRow *) g_owned_rows_head = NULL;
static _Atomic size_t g_owned_rows_count = 0;

void ownedStateRowEnrol(ownedStateRow *row, const char *name,
                        ownedStateKind kind) {
    if (!row) {
        return;
    }
    /* Fast path: already enrolled. Otherwise one CAS winner writes name
     * and kind and pushes the row; every other caller returns without
     * touching a field. */
    /* No order: a caller that sees `enrolled` reads none of the fields the
     * winner writes, and the CAS only elects the one writer of them. */
    /* @sync: relaxed hint */
    if (atomic_load_explicit(&row->enrolled, memory_order_relaxed)) {
        return;
    }
    bool expected = false;
    /* @sync: relaxed owner */
    if (!atomic_compare_exchange_strong_explicit(&row->enrolled, &expected,
                                                 true, memory_order_relaxed,
                                                 memory_order_relaxed)) {
        return;
    }
    /* Sequenced before the release CAS below; a walker reaches the row
     * only through an acquire load of the head. */
    row->name = name;
    row->kind = kind;
    /* @sync: relaxed seed */
    ownedStateRow *head =
        atomic_load_explicit(&g_owned_rows_head, memory_order_relaxed);
    do {
        row->next = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &g_owned_rows_head, &head, row, memory_order_release,
        memory_order_relaxed)); /* @sync: release row.head */
    /* @sync: release row.count */
    atomic_fetch_add_explicit(&g_owned_rows_count, 1, memory_order_release);
}

size_t ownedStateRowCount(void) {
    /* @sync: acquire row.count */
    return atomic_load_explicit(&g_owned_rows_count, memory_order_acquire);
}

const ownedStateRow *ownedStateRowAt(size_t i) {
    /* Rows are never removed and `next` is written before the release
     * CAS that publishes the row, so an acquire load of the head reads a
     * consistent list. */
    /* @sync: acquire row.head */
    const ownedStateRow *r =
        atomic_load_explicit(&g_owned_rows_head, memory_order_acquire);
    while (r && i > 0) {
        r = r->next;
        i--;
    }
    return r;
}

const char *ownedStateKindName(ownedStateKind kind) {
    switch (kind) {
    case OWNED_ROW_ARENA:
        return "arena";
    case OWNED_ROW_CELL:
        return "cell";
    case OWNED_ROW_SLOT:
        return "slot";
    case OWNED_ROW_SEQ:
        return "seq";
    }
    return "unknown";
}

/* ====================================================================
 * ownedSlot
 * ==================================================================== */

/* Bump one row column and return the new value; 0 without a decl or
 * row. */
static uint64_t ownedSlotBump_(const ownedSlot *s, int column) {
    if (!s->decl || !s->decl->row) {
        return 0;
    }
    /* @sync: relaxed counter */
    return atomic_fetch_add_explicit(&s->decl->row->c[column], 1,
                                     memory_order_relaxed) +
           1;
}

static void ownedSlotNote_(const ownedSlot *s, ownedSlotEvent event,
                           ownedSlotState displaced, uint64_t reading) {
    if (s->decl && s->decl->note) {
        s->decl->note(s->decl->noteCtx, s->decl, event, displaced, reading);
    }
}

/* Enrol on every operation (a single acquire load once enrolled). On a
 * single thread the row is listed before any counter moves. Under
 * concurrent first use, a caller that loses the enrolment CAS returns at
 * once and may move a counter before the winner's push lands, so a walk
 * can briefly miss a row whose counters have moved. */
static void ownedSlotEnrol_(const ownedSlot *s) {
    if (s->decl && s->decl->row) {
        ownedStateRowEnrol(s->decl->row, s->decl->name, OWNED_ROW_SLOT);
    }
}

void ownedSlotInit(ownedSlot *s, const ownedSlotDecl *decl) {
    if (!s) {
        return;
    }
    s->decl = decl;
    /* @sync: relaxed init */
    atomic_store_explicit(&s->word, OWNED_SLOT_EMPTY, memory_order_relaxed);
}

/* Internal state, never returned: a Retire displaced CLAIMED and the
 * claimant has not yet released. It reads as RETIRED. The claimant's
 * Publish moves it to RETIRED with a CAS and releases only if that CAS
 * wins, so a Publish that meets plain RETIRED (a thread whose claim was
 * refused, or a claimant publishing again) releases nothing. */
#define OWNED_SLOT_RETIRED_CLAIMED_ 4

static ownedSlotState ownedSlotPublic_(int st) {
    return st == OWNED_SLOT_RETIRED_CLAIMED_ ? OWNED_SLOT_RETIRED
                                             : (ownedSlotState)st;
}

ownedSlotState ownedSlotLoad(const ownedSlot *s) {
    if (!s) {
        return OWNED_SLOT_RETIRED;
    }
    /* @sync: acquire slot.word */
    return ownedSlotPublic_(
        atomic_load_explicit(&((ownedSlot *)s)->word, memory_order_acquire));
}

bool ownedSlotClaim(ownedSlot *s, ownedSlotState *out_prior) {
    if (!s) {
        return false;
    }
    ownedSlotEnrol_(s);
    /* @sync: relaxed seed */
    int st = atomic_load_explicit(&s->word, memory_order_relaxed);
    while (st == OWNED_SLOT_EMPTY || st == OWNED_SLOT_ARMED) {
        /* Acquire: the claimant reads the fields the last Publish wrote.
         * It has written none yet, so nothing needs a release. */
        /* @sync: acquire slot.word */
        if (atomic_compare_exchange_weak_explicit(
                &s->word, &st, OWNED_SLOT_CLAIMED, memory_order_acquire,
                memory_order_relaxed)) {
            if (out_prior) {
                *out_prior = (ownedSlotState)st;
            }
            return true;
        }
    }
    /* CLAIMED (another claimant) or RETIRED: counted and reported. */
    const uint64_t n = ownedSlotBump_(s, OWNED_SLOT_C_CLAIM_REFUSED);
    ownedSlotNote_(s, OWNED_SLOT_C_CLAIM_REFUSED, ownedSlotPublic_(st), n);
    return false;
}

bool ownedSlotPublish(ownedSlot *s, bool still_armed, ownedReleaseFn release,
                      void *cargo) {
    if (!s) {
        return false;
    }
    ownedSlotEnrol_(s);
    int expect = OWNED_SLOT_CLAIMED;
    /* A failure needs no order: only RETIRED_CLAIMED leads on, to the
     * acq_rel CAS below, which synchronizes with the retirer before the
     * release runs. */
    /* @sync: release slot.word */
    if (atomic_compare_exchange_strong_explicit(
            &s->word, &expect,
            still_armed ? OWNED_SLOT_ARMED : OWNED_SLOT_EMPTY,
            memory_order_release, memory_order_relaxed)) {
        return true;
    }
    if (expect != OWNED_SLOT_RETIRED_CLAIMED_) {
        /* No live claim: EMPTY or ARMED (nobody claimed), or RETIRED (a
         * claim refused after a retire, or a claimant whose release has
         * already run). A misuse: leave the slot and the cargo alone. */
        DK_DIAG(DK_DIAG_OWNED_SLOT_UNCLAIMED,
                "ownedSlotPublish with no live claim (slot state %u)",
                (unsigned)expect);
        return false;
    }
    /* A Retire happened while claimed. The CAS to RETIRED decides which
     * Publish releases, so the release runs once even if Publish is
     * repeated; acq_rel also publishes this claimant's field writes. */
    /* @sync: acq_rel slot.word */
    if (!atomic_compare_exchange_strong_explicit(
            &s->word, &expect, OWNED_SLOT_RETIRED, memory_order_acq_rel,
            memory_order_relaxed)) {
        return false;
    }
    /* Whenever a release is supplied, even for a NULL cargo. */
    if (release) {
        release(cargo);
    }
    const uint64_t n = ownedSlotBump_(s, OWNED_SLOT_C_RELEASE_BY_CLAIMANT);
    ownedSlotNote_(s, OWNED_SLOT_C_RELEASE_BY_CLAIMANT, OWNED_SLOT_RETIRED, n);
    return false;
}

ownedSlotState ownedSlotRetire(ownedSlot *s, ownedReleaseFn release,
                               void *cargo) {
    if (!s) {
        return OWNED_SLOT_RETIRED;
    }
    ownedSlotEnrol_(s);
    /* CLAIMED moves to the internal RETIRED_CLAIMED, which the claimant's
     * Publish consumes; EMPTY and ARMED move to RETIRED. A slot already
     * retired is left as it is. */
    /* @sync: relaxed seed */
    int prior = atomic_load_explicit(&s->word, memory_order_relaxed);
    for (;;) {
        if (prior == OWNED_SLOT_RETIRED ||
            prior == OWNED_SLOT_RETIRED_CLAIMED_) {
            return OWNED_SLOT_RETIRED;
        }
        const int next = prior == OWNED_SLOT_CLAIMED
                             ? OWNED_SLOT_RETIRED_CLAIMED_
                             : OWNED_SLOT_RETIRED;
        /* @sync: acq_rel slot.word */
        if (atomic_compare_exchange_weak_explicit(&s->word, &prior, next,
                                                  memory_order_acq_rel,
                                                  memory_order_relaxed)) {
            break;
        }
    }
    switch ((ownedSlotState)prior) {
    case OWNED_SLOT_ARMED:
        /* This caller now owns the fields. */
        if (release) {
            release(cargo);
        }
        break;
    case OWNED_SLOT_CLAIMED: {
        /* The claimant owns the fields; its Publish fails against
         * RETIRED_CLAIMED and releases there. */
        const uint64_t n = ownedSlotBump_(s, OWNED_SLOT_C_RETIRE_MET_CLAIMED);
        ownedSlotNote_(s, OWNED_SLOT_C_RETIRE_MET_CLAIMED, OWNED_SLOT_CLAIMED,
                       n);
        break;
    }
    case OWNED_SLOT_EMPTY:
    case OWNED_SLOT_RETIRED:
        break;
    }
    return (ownedSlotState)prior;
}

static uint64_t ownedSlotRead_(const ownedSlotDecl *decl, int column) {
    if (!decl || !decl->row) {
        return 0;
    }
    /* @sync: relaxed counter */
    return atomic_load_explicit(&decl->row->c[column], memory_order_relaxed);
}

uint64_t ownedSlotRetireMetClaimedTotal(const ownedSlotDecl *decl) {
    return ownedSlotRead_(decl, OWNED_SLOT_C_RETIRE_MET_CLAIMED);
}

uint64_t ownedSlotReleaseByClaimantTotal(const ownedSlotDecl *decl) {
    return ownedSlotRead_(decl, OWNED_SLOT_C_RELEASE_BY_CLAIMANT);
}

uint64_t ownedSlotClaimRefusedTotal(const ownedSlotDecl *decl) {
    return ownedSlotRead_(decl, OWNED_SLOT_C_CLAIM_REFUSED);
}

/* ====================================================================
 * ownedSeq (contract at ownedSeqDecl in ownedState.h)
 *
 * Layout. Version j lives at versions + j * stride, stride =
 * 16 + roundup8(version_bytes): word 0 is the stamp, word 1 is `used`,
 * words 2.. are the payload. Every access to a version is an atomic on
 * one of those words; no pointer into a version leaves this file.
 *
 * Transitions. Exactly three kinds of stamp write exist, two of them
 * RMWs; readers never write a stamp, and a writer that does not hold
 * the version writes nothing:
 *
 *   T0  -          -> 0        plain store (zero fill)  by the owner,
 *                                                       before the
 *                                                       instance is
 *                                                       reachable
 *   T1  standing   -> 4n + 1   CAS_strong acquire       writer n, entry;
 *       (tag not INPROGRESS                             refused when the
 *        and standing >> 2 < n)                         version is held
 *                                                       or newer: the
 *                                                       writer abandons
 *   T2  4n + 1     -> 4n       CAS_strong release       the same writer,
 *                                                       exit
 *
 * `cursor` moves by a monotone-max CAS, release, only after a T2 that
 * succeeded. Because T1 requires a lower standing number, the number in
 * a version's stamp only grows, so a delayed writer cannot replace a
 * newer completed publication with an older one.
 *
 * Exclusivity. A payload word of version j is stored only by a writer
 * whose T1 succeeded on j, and only between that T1 and its own T2. T1
 * succeeds only from a non-INPROGRESS stamp, so two claim windows on j
 * never overlap in the stamp's modification order, and every store of
 * the earlier holder is sequenced before its T2, which precedes the
 * later holder's T1 in that order and, by the window-order step below,
 * happens before the later holder's stores. Because a writer that cannot
 * enter writes nothing, "exactly one publication was inside the
 * version" is a statement about payload stores and not only about
 * stamps.
 *
 * Safety. A version reads VALID for `want` iff R1 == R2 == 4 * want.
 * The value 4 * want is written by exactly one instruction, publication
 * `want`'s own T2, which runs at most once. Every payload store inside
 * `want`'s window is `want`'s own (exclusivity), so an accepted read
 * witnesses a version that exactly one publication wrote for the whole
 * of its occupancy, and the ordering argument below shows the copy is
 * that publication's.
 *
 * Liveness. The holder's T2 always lands: nothing else writes the stamp
 * while it is INPROGRESS (readers load; an entering writer refuses on
 * INPROGRESS), so the strong CAS finds 4n + 1 and writes 4n. The next
 * writer with a higher number finds tag PUBLISHED and enters, so the
 * version returns to service after every publication. A weak CAS could
 * fail spuriously on that exit and leave the version INPROGRESS, which
 * is why the exit CAS is strong. A writer that abandoned did not enter,
 * so it owes no exit.
 *
 * Ordering, per version. Writer: T1 is a CAS with acquire on success,
 * every store to `used` and to a payload word is a release store, and T2
 * is a CAS with release on success. Reader: R1, the load of `used`, every
 * payload load (C) and R2 are acquire loads. P(x) names publication x's
 * stores to `used` and the payload. The argument uses only
 * happens-before and the coherence rules of C11 5.1.2.4, not the single
 * total order of seq_cst operations, so these orders suffice (and on
 * x86-64 a release store is a plain mov where a seq_cst store is an
 * xchg).
 *   Window order: if holder a's window precedes holder b's on a version,
 *   every store of a happens before every store of b. Every stamp
 *   modification after the zero fill is an RMW (a successful T1 or T2),
 *   so the stamp's modification order from T2(a) on is a release
 *   sequence headed by T2(a). T1(b) reads a value in that sequence and is
 *   an acquire, so T2(a) synchronizes with T1(b); a's stores are
 *   sequenced before T2(a) and b's after T1(b). By write-write coherence,
 *   each word's modification order then lists the holders' stores in
 *   window order.
 *   Lower bound: R1 read 4s, which only T2(s) writes. T2(s) is a release
 *   and R1 an acquire, so P(s), sequenced before T2(s), happens before
 *   every C, sequenced after R1. By write-read coherence each C reads s's
 *   last store to its word or a store later in that word's modification
 *   order, which by window order belongs to s or to a later holder.
 *   Upper bound: suppose C read a store of P(m) for a later holder m.
 *   That store is a release and C an acquire, so T1(m), sequenced before
 *   it, happens before C and so before R2. By write-read coherence R2
 *   reads T1(m)'s 4m + 1 or a later stamp. T1(m) follows T2(s) in the
 *   stamp's modification order and 4s is written only by T2(s), so R2
 *   cannot have read 4s. Contradiction: every C read s's last store.
 *   Premise: 4s appears exactly once in the version's modification
 *   order (an entry writes 4m + 1, nothing else writes a stamp), so
 *   "R2 read 4s" names one point in that order. The argument does not
 *   rely on numbers entering in order: fetch_add on `assigned` orders
 *   numbers, not entries, and under concurrent writers the holder of
 *   s + N can enter before the holder of s, which then abandons.
 *   Each order carries one edge: T1 acquire for window order; T2 release
 *   and R1 acquire for the lower bound; release payload stores and
 *   acquire payload loads for the upper bound. R2 needs none (C is
 *   sequenced before it); it is an acquire for symmetry. A word below
 *   `used` that s never stored has no lower bound, so C may read an
 *   older holder's store: hence the fill contract in the header.
 *
 * ThreadSanitizer silence is a separate fact: it never reports two
 * atomic accesses, whatever their orders. The orders above are what the
 * value argument needs.
 * ==================================================================== */

uint64_t ownedSeqPublishedTotal(void) {
    return ownedTotalRead_(OWNED_T_SEQ_PUBLISHED);
}
uint64_t ownedSeqTornReadsTotal(void) {
    return ownedTotalRead_(OWNED_T_SEQ_TORN_READS);
}
uint64_t ownedSeqOverwrittenReadsTotal(void) {
    return ownedTotalRead_(OWNED_T_SEQ_OVERWRITTEN_READS);
}
uint64_t ownedSeqPublishAbandonedTotal(void) {
    return ownedTotalRead_(OWNED_T_SEQ_PUBLISH_ABANDONED);
}
uint64_t ownedSeqPublishRefusedOversizeTotal(void) {
    return ownedTotalRead_(OWNED_T_SEQ_REFUSED_OVERSIZE);
}

uint64_t ownedSeqColumnTotal(const ownedSeqDecl *decl, ownedSeqColumn column) {
    if (!decl || !decl->row || (int)column < 0 ||
        (int)column >= OWNED_ROW_COUNTER_MAX) {
        return 0;
    }
    /* @sync: relaxed counter */
    return atomic_load_explicit(&decl->row->c[column], memory_order_relaxed);
}

struct ownedSeqVersionRef {
    _Atomic uint64_t *words; /* the payload words of one version */
    size_t capacity;         /* version_bytes */
};

static size_t ownedSeqStride_(const ownedSeqDecl *decl) {
    return OWNED_SEQ_STORAGE_WORDS(1u, decl->version_bytes) * sizeof(uint64_t);
}

/* An instance that can run the protocol: a declaration, storage and at
 * least one version. */
static bool ownedSeqArmed_(const ownedSeq *s) {
    return s && s->decl && s->versions && s->decl->versions != 0;
}

static _Atomic uint64_t *ownedSeqVersion_(const ownedSeq *s, uint64_t index) {
    return (_Atomic uint64_t *)((uint8_t *)s->versions +
                                (size_t)index * ownedSeqStride_(s->decl));
}

/* The row's counters are one shared line per declaration: instances that
 * share a row pay for it under concurrent use, as the header notes. */
static void ownedSeqBump_(const ownedSeq *s, ownedSeqColumn column,
                          ownedTotal_ total) {
    ownedTotalAdd_(total);
    if (s->decl->row) {
        /* @sync: relaxed counter */
        atomic_fetch_add_explicit(&s->decl->row->c[column], 1,
                                  memory_order_relaxed);
    }
}

static void ownedSeqEnrol_(const ownedSeq *s) {
    if (s->decl->row) {
        ownedStateRowEnrol(s->decl->row, s->decl->name, OWNED_ROW_SEQ);
    }
}

/* Count and report a publication by a disallowed writer; never refuse
 * it. Shares the ownedCell violation counter. */
static void ownedSeqWriterCheck_(const ownedSeq *s) {
    const ownedSeqDecl *const d = s->decl;
    if (!d->writer_ok) {
        return;
    }
    if (d->writer_ok(s->owner)) {
        return;
    }
    ownedTotalAdd_(OWNED_T_WRITER_VIOLATION);
    DK_DIAG(DK_DIAG_OWNED_WRITER_VIOLATION,
            "sequence %s published by a writer its declaration does not "
            "allow",
            d->name ? d->name : "(unnamed)");
    if (d->note_violation) {
        d->note_violation(d, s->owner);
    }
}

size_t ownedSeqStorageBytes(const ownedSeqDecl *decl) {
    if (!decl || decl->versions == 0) {
        return 0;
    }
    return (size_t)decl->versions * ownedSeqStride_(decl);
}

void ownedSeqInit(ownedSeq *s, const ownedSeqDecl *decl, void *owner,
                  void *storage) {
    if (!s) {
        return;
    }
    s->decl = decl;
    s->owner = owner;
    s->versions = storage;
    /* @sync: relaxed init */
    atomic_store_explicit(&s->assigned, 0, memory_order_relaxed);
    /* @sync: relaxed init */
    atomic_store_explicit(&s->cursor, 0, memory_order_relaxed);
    if (decl && storage && decl->versions != 0) {
        /* T0: the one plain store of a stamp, sequenced before the
         * instance becomes reachable (lifecycle contract). */
        memset(storage, 0, ownedSeqStorageBytes(decl));
    }
}

void ownedSeqDestruct(ownedSeq *s) {
    if (!s) {
        return;
    }
    s->decl = NULL;
    s->versions = NULL;
    s->owner = NULL;
}

void ownedSeqPutWord(ownedSeqVersionRef *v, size_t wordIndex, uint64_t w) {
    if (!v || wordIndex >= (v->capacity + 7u) / 8u) {
        return;
    }
    /* @sync: release seq.payload */
    atomic_store_explicit(&v->words[wordIndex], w, memory_order_release);
}

void ownedSeqPutBytes(ownedSeqVersionRef *v, size_t offset, const void *src,
                      size_t len) {
    if (!v || !src || offset > v->capacity || len > v->capacity - offset) {
        return;
    }
    /* Merge partial words; every store is a release, as the ordering
     * argument requires. The merge load reads this writer's own claimed
     * version, so it needs no ordering. */
    const uint8_t *in = (const uint8_t *)src;
    size_t pos = offset;
    size_t remaining = len;
    while (remaining > 0) {
        const size_t wi = pos / 8u;
        const size_t within = pos % 8u;
        const size_t take =
            (8u - within) < remaining ? (8u - within) : remaining;
        uint64_t w = 0;
        if (take != 8u) {
            /* A partial word merges with what stands; a whole word
             * replaces it and needs no read. */
            /* @sync: relaxed owner */
            w = atomic_load_explicit(&v->words[wi], memory_order_relaxed);
        }
        memcpy((uint8_t *)&w + within, in, take);
        /* @sync: release seq.payload */
        atomic_store_explicit(&v->words[wi], w, memory_order_release);
        in += take;
        pos += take;
        remaining -= take;
    }
}

typedef struct ownedSeqFlatFill_ {
    const void *src;
    size_t len;
} ownedSeqFlatFill_;

static void ownedSeqFlatFillFn_(ownedSeqVersionRef *v, void *ctx) {
    const ownedSeqFlatFill_ *const f = ctx;
    ownedSeqPutBytes(v, 0, f->src, f->len);
}

bool ownedSeqPublish(ownedSeq *s, const void *src, size_t len) {
    if (!src && len > 0) {
        /* Nothing to copy: refuse before a number is assigned rather
         * than publish stale bytes as VALID. */
        return false;
    }
    ownedSeqFlatFill_ f = {.src = src, .len = len};
    return ownedSeqPublishWith(s, len, ownedSeqFlatFillFn_, &f);
}

/* Monotone max, as ownedArenaExit does for idleEpoch. */
static void ownedSeqCursorAdvance_(ownedSeq *s, uint64_t n) {
    /* @sync: relaxed seed */
    uint64_t prev = atomic_load_explicit(&s->cursor, memory_order_relaxed);
    /* @sync: release seq.cursor */
    while (prev < n && !atomic_compare_exchange_weak_explicit(
                           &s->cursor, &prev, n, memory_order_release,
                           memory_order_relaxed)) {
        /* prev reloaded by the exchange. */
    }
}

bool ownedSeqPublishWith(ownedSeq *s, size_t len, ownedSeqFillFn fill,
                         void *ctx) {
    if (!fill && len > 0) {
        /* Nothing would write the bytes: refuse before a number is
         * assigned rather than publish stale bytes as VALID. */
        return false;
    }
    if (!ownedSeqArmed_(s)) {
        return false; /* zero versions: nothing to write into */
    }
    ownedSeqWriterCheck_(s);
    ownedSeqEnrol_(s);
    if (len > s->decl->version_bytes) {
        /* Refused before a number is assigned: no version consumed. */
        ownedSeqBump_(s, OWNED_SEQ_C_REFUSED_OVERSIZE,
                      OWNED_T_SEQ_REFUSED_OVERSIZE);
        return false;
    }
    /* T1: enter by CAS from a stamp no writer holds and whose number is
     * below n. A writer that finds the version in progress, finds a
     * publication numbered n or later (it was delayed past a whole lap),
     * or loses the CAS, abandons without writing, so a version's payload
     * is only stored by its current holder and a newer publication is
     * never replaced by an older one. */
    /* @sync: relaxed counter */
    const uint64_t n =
        atomic_fetch_add_explicit(&s->assigned, 1, memory_order_relaxed) + 1;
    _Atomic uint64_t *const v =
        ownedSeqVersion_(s, (n - 1) % s->decl->versions);
    const uint64_t claim = 4u * n + OWNED_SEQ_TAG_INPROGRESS;
    /* The load only seeds the CAS, which compares against the stamp as it
     * stands; the acquire that window order needs is the CAS's own. */
    /* @sync: relaxed seed */
    uint64_t standing = atomic_load_explicit(&v[0], memory_order_relaxed);
    /* @sync: acquire seq.stamp */
    if ((standing & 3u) == OWNED_SEQ_TAG_INPROGRESS || (standing >> 2) >= n ||
        !atomic_compare_exchange_strong_explicit(&v[0], &standing, claim,
                                                 memory_order_acquire,
                                                 memory_order_relaxed)) {
        /* Number n is consumed and never published. A reader asking for
         * it reads TORN while the version holds an older publication and
         * OVERWRITTEN once it holds a newer one. The holder's
         * publication is untouched. */
        ownedSeqBump_(s, OWNED_SEQ_C_PUBLISH_ABANDONED,
                      OWNED_T_SEQ_PUBLISH_ABANDONED);
        return true;
    }
    /* Fill: release stores into `used` and the payload, all inside this
     * writer's claim window. */
    /* @sync: release seq.payload */
    atomic_store_explicit(&v[1], (uint64_t)len, memory_order_release);
    ownedSeqVersionRef ref = {.words = &v[OWNED_SEQ_HEADER_WORDS],
                              .capacity = s->decl->version_bytes};
    if (fill) {
        fill(&ref, ctx);
    }
    /* T2: strong CAS from the claim. Only the holder writes this stamp
     * while it is INPROGRESS, so the CAS finds the claim; strong, so a
     * spurious failure cannot leave the version INPROGRESS. */
    uint64_t expected = claim;
    /* @sync: release seq.stamp */
    if (atomic_compare_exchange_strong_explicit(&v[0], &expected, 4u * n,
                                                memory_order_release,
                                                memory_order_relaxed)) {
        ownedSeqCursorAdvance_(s, n);
        ownedSeqBump_(s, OWNED_SEQ_C_PUBLISHED, OWNED_T_SEQ_PUBLISHED);
        return true;
    }
    /* Unreachable given exclusivity; counted rather than asserted. */
    ownedSeqBump_(s, OWNED_SEQ_C_PUBLISH_ABANDONED,
                  OWNED_T_SEQ_PUBLISH_ABANDONED);
    return true;
}

ownedSeqReadResult ownedSeqReadMissed(ownedSeq *s, ownedSeqReadResult r) {
    if (!ownedSeqArmed_(s)) {
        return OWNED_SEQ_OVERWRITTEN;
    }
    ownedSeqEnrol_(s);
    if (r == OWNED_SEQ_TORN) {
        ownedSeqBump_(s, OWNED_SEQ_C_TORN_READS, OWNED_T_SEQ_TORN_READS);
        return r;
    }
    ownedSeqBump_(s, OWNED_SEQ_C_OVERWRITTEN_READS,
                  OWNED_T_SEQ_OVERWRITTEN_READS);
    return OWNED_SEQ_OVERWRITTEN;
}

uint64_t ownedSeqCursor(const ownedSeq *s) {
    /* @sync: acquire seq.cursor */
    return s ? atomic_load_explicit(&s->cursor, memory_order_acquire) : 0;
}

uint64_t ownedSeqAssigned(const ownedSeq *s) {
    /* A count: its only writer is the relaxed fetch_add that hands out
     * numbers, so there is nothing to acquire. */
    /* @sync: relaxed counter */
    return s ? atomic_load_explicit(&s->assigned, memory_order_relaxed) : 0;
}

uint32_t ownedSeqVersions(const ownedSeq *s) {
    return (s && s->decl && s->versions) ? s->decl->versions : 0;
}

bool ownedSeqReadCanSucceed(const ownedSeq *s, uint64_t assignedBefore) {
    const uint32_t n = ownedSeqVersions(s);
    if (n < 2u) {
        return false; /* no slack, and no instance at 0 */
    }
    return (ownedSeqAssigned(s) - assignedBefore) < (uint64_t)(n - 1u);
}

ownedSeqReadResult ownedSeqReadAt(ownedSeq *s, uint64_t want, void *dst,
                                  size_t dstCap, size_t *outLen) {
    if (outLen) {
        *outLen = 0;
    }
    if (!ownedSeqArmed_(s)) {
        return OWNED_SEQ_OVERWRITTEN;
    }
    ownedSeqEnrol_(s);
    if (want == 0) {
        ownedSeqBump_(s, OWNED_SEQ_C_OVERWRITTEN_READS,
                      OWNED_T_SEQ_OVERWRITTEN_READS);
        return OWNED_SEQ_OVERWRITTEN;
    }
    _Atomic uint64_t *const v =
        ownedSeqVersion_(s, (want - 1) % s->decl->versions);
    const uint64_t published = 4u * want;
    /* R1. */
    /* @sync: acquire seq.stamp */
    const uint64_t v1 = atomic_load_explicit(&v[0], memory_order_acquire);
    if (v1 != published) {
        /* TORN when `want` is in progress or the version still holds an
         * older number (its writer has not entered yet, or abandoned, in
         * which case it reads TORN until the ring laps the version);
         * OVERWRITTEN when a newer number stands. */
        const bool torn =
            (v1 == published + OWNED_SEQ_TAG_INPROGRESS) || (v1 < published);
        if (torn) {
            ownedSeqBump_(s, OWNED_SEQ_C_TORN_READS, OWNED_T_SEQ_TORN_READS);
            return OWNED_SEQ_TORN;
        }
        ownedSeqBump_(s, OWNED_SEQ_C_OVERWRITTEN_READS,
                      OWNED_T_SEQ_OVERWRITTEN_READS);
        return OWNED_SEQ_OVERWRITTEN;
    }
    /* C: `used` first, then the payload words. */
    /* @sync: acquire seq.payload */
    const uint64_t used = atomic_load_explicit(&v[1], memory_order_acquire);
    if (used > (uint64_t)dstCap || used > (uint64_t)s->decl->version_bytes) {
        /* A shortfall is trustworthy only if the stamp still stands:
         * validate (R2) before reporting a length. */
        /* @sync: acquire seq.stamp */
        const uint64_t v2 = atomic_load_explicit(&v[0], memory_order_acquire);
        if (v2 == published && used <= (uint64_t)s->decl->version_bytes) {
            if (outLen) {
                *outLen = (size_t)used;
            }
            return OWNED_SEQ_CAPACITY;
        }
        ownedSeqBump_(s, OWNED_SEQ_C_OVERWRITTEN_READS,
                      OWNED_T_SEQ_OVERWRITTEN_READS);
        return OWNED_SEQ_OVERWRITTEN;
    }
    {
        uint8_t *out = (uint8_t *)dst;
        size_t remaining = (size_t)used;
        size_t wi = 0;
        /* Copy whole words with a constant-size memcpy (one store each)
         * and only the tail with a variable-length one. */
        while (remaining >= 8u) {
            /* @sync: acquire seq.payload */
            const uint64_t w = atomic_load_explicit(
                &v[OWNED_SEQ_HEADER_WORDS + wi], memory_order_acquire);
            memcpy(out, &w, 8u);
            out += 8u;
            remaining -= 8u;
            wi++;
        }
        if (remaining > 0) {
            /* @sync: acquire seq.payload */
            const uint64_t w = atomic_load_explicit(
                &v[OWNED_SEQ_HEADER_WORDS + wi], memory_order_acquire);
            memcpy(out, &w, remaining);
        }
    }
    /* R2; the reader writes nothing (see the ordering argument above). */
    /* @sync: acquire seq.stamp */
    const uint64_t v2 = atomic_load_explicit(&v[0], memory_order_acquire);
    if (v2 != published) {
        ownedSeqBump_(s, OWNED_SEQ_C_OVERWRITTEN_READS,
                      OWNED_T_SEQ_OVERWRITTEN_READS);
        return OWNED_SEQ_OVERWRITTEN;
    }
    if (outLen) {
        *outLen = (size_t)used;
    }
    return OWNED_SEQ_VALID;
}
