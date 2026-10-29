/* persistContractTest.c - the persistence contract (see persistContract.h)
 * applied to every registered persistOps descriptor. */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../cleaner.h"
#include "../../ctest.h"
#include "../../datakit.h"
#include "../../dkC23.h" /* stdc_bit_width */
#include "../../str.h"
#include "../../timeUtil.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

defcleanerName(uint8_t *, zfree, Bytes);

/* ====================================================================
 * Registry: one entry per persistOps descriptor (declared in
 * persistContract.h)
 * ==================================================================== */
static const persistContractType *const pcRegistry[] = {
    &persistContractFlex,
    &persistContractMultimap,
    &persistContractMultiroar,
    &persistContractIntset,
    &persistContractIntsetBig,
    &persistContractHyperloglog,
    &persistContractMultiarray,
    &persistContractMultilist,
    &persistContractMultidict,
    &persistContractMultiOrderedSet,
    &persistContractMultiLRU,
    &persistContractHmacKeyring,
    &persistContractClusterMailbox,
    &persistContractClusterRing,
    &persistContractAeadNonceLog,
    &persistContractFenwickI16,
    &persistContractFenwickI32,
    &persistContractFenwickI64,
    &persistContractFenwickI128,
    &persistContractFenwickU16,
    &persistContractFenwickU32,
    &persistContractFenwickU64,
    &persistContractFenwickU128,
    &persistContractFenwickFloat,
    &persistContractFenwickDouble,
    &persistContractFenwick2dI16,
    &persistContractFenwick2dI32,
    &persistContractFenwick2dI64,
    &persistContractFenwick2dI128,
    &persistContractFenwick2dU16,
    &persistContractFenwick2dU32,
    &persistContractFenwick2dU64,
    &persistContractFenwick2dU128,
    &persistContractFenwick2dFloat,
    &persistContractFenwick2dDouble,
    &persistContractMultiFenwick,
    &persistContractMultiFenwickFlat,
    &persistContractMultiFenwick2d,
    &persistContractSegmentI16,
    &persistContractSegmentI32,
    &persistContractSegmentI64,
    &persistContractSegmentI128,
    &persistContractSegmentU16,
    &persistContractSegmentU32,
    &persistContractSegmentU64,
    &persistContractSegmentU128,
    &persistContractSegmentFloat,
    &persistContractSegmentDouble,
    &persistContractMultimapAtom,
    &persistContractMultimapAtomDense,
    &persistContractStringPool,
    &persistContractAtomPool,
    &persistContractIdRows,
};

/* ====================================================================
 * The log a generator records into
 * ==================================================================== */
typedef struct pcEntry {
    persistOp op;
    uint8_t *data;
    size_t len;
} pcEntry;

struct persistContractLog {
    const persistOps *ops;
    uint8_t *start;
    size_t startLen;
    bool started;
    pcEntry *entries;
    size_t count;
    size_t cap;
};

void persistContractStart(persistContractLog *log, const void *structure) {
    zfree(log->start);
    /* @own: container; free=zfree */
    log->start = log->ops->snapshot(structure, &log->startLen);
    log->started = true;
}

void persistContractRecord(persistContractLog *log, persistOp op,
                           const void *args, size_t argc) {
    if (log->count == log->cap) {
        log->cap = log->cap ? log->cap * 2 : 64;
        /* @own: container; free=zfree */
        log->entries = zrealloc(log->entries, log->cap * sizeof(pcEntry));
    }
    size_t len = 0;
    /* @own: container; free=zfree */
    uint8_t *data = log->ops->encodeOp(op, args, argc, &len);
    log->entries[log->count++] = (pcEntry){.op = op, .data = data, .len = len};
}

static void pcLogFree(persistContractLog *log) {
    for (size_t i = 0; i < log->count; i++) {
        zfree(log->entries[i].data);
    }
    zfree(log->entries);
    zfree(log->start);
    *log = (persistContractLog){};
}

/* ====================================================================
 * Values
 * ==================================================================== */
void persistContractValue(uint64_t *rng, databoxBig *out, uint8_t *buf,
                          size_t bufLen) {
    static constexpr char chars[] = "0123456789-.e abcxyz";
    const uint64_t r = splitmix64(rng);
    const uint64_t s = splitmix64(rng);
    *out = (databoxBig){};
    switch (r % 12) {
    case 0:
        *out = DATABOX_BIG_SIGNED((int64_t)(s % 256) - 128);
        break;
    case 1:
        *out = DATABOX_BIG_SIGNED((int64_t)s);
        break;
    case 2:
        *out = DATABOX_BIG_UNSIGNED(s);
        break;
    case 3:
        out->type = DATABOX_DOUBLE_64;
        out->data.d64 = (double)(int64_t)s / 1024.0;
        break;
    case 4:
        DATABOX_SET_FLOAT((databox *)out, (float)(int32_t)s / 8.0f);
        break;
    case 5:
        DATABOX_BIG_SIGNED_128(
            out, (__int128_t)(((__uint128_t)s << 64) | splitmix64(rng)));
        break;
    case 6:
        DATABOX_BIG_UNSIGNED_128(out, ((__uint128_t)s << 64) | splitmix64(rng));
        break;
    case 7:
        *(databox *)out = (s % 3 == 0)   ? DATABOX_BOX_TRUE
                          : (s % 3 == 1) ? DATABOX_BOX_FALSE
                                         : DATABOX_BOX_NULL;
        break;
    default: {
        /* Bytes: digit-heavy, so some read as numerals */
        const size_t max = bufLen < 48 ? bufLen : 48;
        const size_t len = max ? (size_t)(s % (max + 1)) : 0;
        for (size_t i = 0; i < len; i++) {
            buf[i] = (uint8_t)chars[splitmix64(rng) % (sizeof(chars) - 1)];
        }
        *(databox *)out = DATABOX_WITH_BYTES(buf, len);
        break;
    }
    }
}

bool persistContractSameBytes(const databox *got, const databox *want) {
    const uint8_t *g = nullptr;
    if (got->type == DATABOX_BYTES) {
        g = got->data.bytes.start;
    } else if (got->type == DATABOX_BYTES_EMBED) {
        g = got->data.bytes.embed;
    } else {
        return false;
    }
    return got->len == want->len &&
           (got->len == 0 || memcmp(g, want->data.bytes.start, got->len) == 0);
}

/* ====================================================================
 * Checks
 * ==================================================================== */
typedef struct pcRow {
    size_t elements;    /* of the largest build */
    size_t snapshotLen; /* of the largest build */
    uint64_t snapshotNs;
    uint64_t restoreNs;
    size_t opsReplayed;
    size_t damagedRefused;
    size_t damagedAccepted;
    size_t damagedOpsRefused;
    size_t damagedOpsAccepted;
    int failures;
    char at[64]; /* the build being checked, for failure lines */
} pcRow;

#define PC_FAIL(row, ops, what, ...)                                           \
    do {                                                                       \
        if ((row)->failures++ < 10) {                                          \
            printf("persistContract %s [%s]: " what "\n", (ops)->name,         \
                   (row)->at, __VA_ARGS__);                                    \
            fflush(stdout);                                                    \
        }                                                                      \
    } while (0)

/* Restore from an exact-size heap copy, so sanitizers see any read past
 * the snapshot's end. */
static void *pcRestore(const persistOps *ops, const uint8_t *data, size_t len) {
    cleanerName(zfree, Bytes) uint8_t *copy = zmalloc(len ? len : 1);
    if (len) {
        memcpy(copy, data, len);
    }
    return ops->restore(copy, len);
}

static bool pcSame(const uint8_t *a, size_t aLen, const uint8_t *b,
                   size_t bLen) {
    return aLen == bLen && (aLen == 0 || memcmp(a, b, aLen) == 0);
}

/* Contract 1 for structure `s`; `snap` receives s's snapshot when
 * non-NULL (caller frees). */
static bool pcRoundTrip(pcRow *row, const persistOps *ops, const void *s,
                        const char *what, uint8_t **snap, size_t *snapLen) {
    size_t aLen = 0;
    uint8_t *a = ops->snapshot(s, &aLen);
    if (!a) {
        PC_FAIL(row, ops, "%s: snapshot returned NULL", what);
        return false;
    }
    bool ok = true;
    void *r = pcRestore(ops, a, aLen);
    if (!r) {
        PC_FAIL(row, ops, "%s: restore refused its own snapshot (%zu B)", what,
                aLen);
        ok = false;
    } else {
        size_t bLen = 0;
        uint8_t *b = ops->snapshot(r, &bLen);
        if (!b || !pcSame(a, aLen, b, bLen)) {
            PC_FAIL(row, ops, "%s: re-snapshot differs (%zu B, then %zu B)",
                    what, aLen, bLen);
            ok = false;
        }
        zfree(b);
        if (ops->count(r) != ops->count(s)) {
            PC_FAIL(row, ops, "%s: count %zu restored as %zu", what,
                    ops->count(s), ops->count(r));
            ok = false;
        }
        if (ops->validate && !ops->validate(r)) {
            PC_FAIL(row, ops, "%s: validate() false after restore", what);
            ok = false;
        }
        ops->free(r);
    }
    if (snap && ok) {
        *snap = a;
        *snapLen = aLen;
    } else {
        zfree(a);
    }
    return ok;
}

/* Apply log entries [from, to) to *s; false when applyOp refuses one. */
static bool pcReplay(pcRow *row, const persistOps *ops, void **s,
                     const persistContractLog *log, size_t from, size_t to,
                     const char *what) {
    for (size_t i = from; i < to; i++) {
        const pcEntry *e = &log->entries[i];
        if (!ops->applyOp(s, e->op, e->data, e->len)) {
            PC_FAIL(row, ops, "%s: applyOp refused op %d (entry %zu, %zu B)",
                    what, (int)e->op, i, e->len);
            return false;
        }
        row->opsReplayed++;
    }
    return true;
}

/* Snapshot `s` and compare with `want`; frees s. */
static void pcExpect(pcRow *row, const persistOps *ops, void *s,
                     const uint8_t *want, size_t wantLen, const char *what) {
    size_t len = 0;
    uint8_t *got = ops->snapshot(s, &len);
    if (!got || !pcSame(got, len, want, wantLen)) {
        PC_FAIL(row, ops, "%s: snapshot differs from live (%zu B vs %zu B)",
                what, len, wantLen);
    }
    zfree(got);
    ops->free(s);
}

/* Contract 2 */
static void pcWal(pcRow *row, const persistOps *ops,
                  const persistContractLog *log, const uint8_t *live,
                  size_t liveLen) {
    if (!log->started || !log->start) {
        PC_FAIL(row, ops, "%s", "generator recorded no starting snapshot");
        return;
    }

    /* Onto the restored starting structure */
    void *s = pcRestore(ops, log->start, log->startLen);
    if (!s) {
        PC_FAIL(row, ops, "%s", "starting snapshot does not restore");
        return;
    }
    if (pcReplay(row, ops, &s, log, 0, log->count, "replay from start")) {
        pcExpect(row, ops, s, live, liveLen, "replay from start");
    } else {
        ops->free(s);
    }

    /* Onto newEmpty(), when that is the starting structure (a type with
     * no meaningful empty state has no newEmpty; see persistOps) */
    if (ops->newEmpty) {
        void *e = ops->newEmpty();
        size_t eLen = 0;
        uint8_t *eSnap = e ? ops->snapshot(e, &eLen) : nullptr;
        if (!e || !eSnap) {
            PC_FAIL(row, ops, "%s", "newEmpty() gives no snapshot");
            if (e) {
                ops->free(e);
            }
        } else if (pcSame(eSnap, eLen, log->start, log->startLen)) {
            if (pcReplay(row, ops, &e, log, 0, log->count,
                         "replay onto newEmpty")) {
                pcExpect(row, ops, e, live, liveLen, "replay onto newEmpty");
            } else {
                ops->free(e);
            }
        } else {
            ops->free(e);
        }
        zfree(eSnap);
    }

    /* First half replayed, snapshotted and restored, then the second */
    const size_t half = log->count / 2;
    void *h = pcRestore(ops, log->start, log->startLen);
    if (!h || !pcReplay(row, ops, &h, log, 0, half, "first half")) {
        if (h) {
            ops->free(h);
        }
        return;
    }
    size_t midLen = 0;
    uint8_t *mid = ops->snapshot(h, &midLen);
    ops->free(h);
    void *m = mid ? pcRestore(ops, mid, midLen) : nullptr;
    zfree(mid);
    if (!m) {
        PC_FAIL(row, ops, "%s", "mid-point snapshot does not restore");
        return;
    }
    if (pcReplay(row, ops, &m, log, half, log->count, "second half")) {
        pcExpect(row, ops, m, live, liveLen, "mid-point snapshot + replay");
    } else {
        ops->free(m);
    }
}

/* Contract 3 on one damaged snapshot */
static void pcDamaged(pcRow *row, const persistOps *ops, const uint8_t *data,
                      size_t len, const char *what, size_t at) {
    void *r = pcRestore(ops, data, len);
    if (!r) {
        row->damagedRefused++;
        return;
    }
    row->damagedAccepted++;
    char label[96];
    snprintf(label, sizeof(label), "%s at %zu", what, at);
    pcRoundTrip(row, ops, r, label, nullptr, nullptr);
    ops->free(r);
}

static void pcDamage(pcRow *row, const persistOps *ops, const uint8_t *snap,
                     size_t len, uint64_t seed) {
    for (size_t t = 0; t < len; t++) {
        pcDamaged(row, ops, snap, t, "truncated", t);
    }
    if (len == 0) {
        return;
    }
    cleanerName(zfree, Bytes) uint8_t *copy = zmalloc(len);
    memcpy(copy, snap, len);
    uint64_t rng = seed;
    /* Every byte for small snapshots, a seeded 4096 otherwise; four
     * masks each: low bit, high bit, all bits, random */
    const size_t positions = len <= 4096 ? len : 4096;
    for (size_t p = 0; p < positions; p++) {
        const size_t at = len <= 4096 ? p : (size_t)(splitmix64(&rng) % len);
        const uint8_t masks[4] = {0x01, 0x80, 0xFF,
                                  (uint8_t)(splitmix64(&rng) | 1)};
        for (size_t k = 0; k < 4; k++) {
            copy[at] ^= masks[k];
            pcDamaged(row, ops, copy, len, "byte flipped", at);
            copy[at] ^= masks[k];
        }
    }
}

/* Contract 5 on one damaged op body, applied to the structure restored
 * from `state`: refused or applied, the structure meets (1) */
static void pcDamagedOp(pcRow *row, const persistOps *ops, const uint8_t *state,
                        size_t stateLen, const pcEntry *e, const uint8_t *body,
                        size_t len, const char *what, size_t at) {
    void *s = pcRestore(ops, state, stateLen);
    if (!s) {
        PC_FAIL(row, ops, "%s",
                "op damage: the replayed state does not restore");
        return;
    }
    /* An exact-size heap copy, so sanitizers see any read past the body */
    cleanerName(zfree, Bytes) uint8_t *copy = zmalloc(len ? len : 1);
    if (len) {
        memcpy(copy, body, len);
    }
    if (ops->applyOp(&s, e->op, copy, len)) {
        row->damagedOpsAccepted++;
    } else {
        row->damagedOpsRefused++;
    }
    char label[96];
    snprintf(label, sizeof(label), "op %d %s at %zu", (int)e->op, what, at);
    pcRoundTrip(row, ops, s, label, nullptr, nullptr);
    ops->free(s);
}

/* Contract 5: for up to 8 entries spread over the log, every truncation
 * and bit-clearing corruptions (every byte of a body up to 8 bytes, 8
 * seeded positions of a longer one; its lowest set bit, its highest set
 * bit, all of it) of the entry's body, applied right where the entry
 * belongs.  Clearing bits never grows a numeric operand: a damaged body
 * that still decodes asks no more work than the op it came from (a
 * range op's extent cannot turn into 2^56 bits) */
static void pcDamageOps(pcRow *row, const persistOps *ops,
                        const persistContractLog *log, uint64_t seed) {
    if (!log->start || log->count == 0) {
        return;
    }
    uint64_t rng = seed;
    const size_t picks = log->count < 8 ? log->count : 8;
    for (size_t k = 0; k < picks; k++) {
        const size_t idx = k * log->count / picks;
        const pcEntry *e = &log->entries[idx];

        /* The state the entry applies to: the start plus entries before it */
        void *s = pcRestore(ops, log->start, log->startLen);
        for (size_t i = 0; s && i < idx; i++) {
            const pcEntry *p = &log->entries[i];
            if (!ops->applyOp(&s, p->op, p->data, p->len)) {
                PC_FAIL(row, ops, "op damage: entry %zu does not replay", i);
                ops->free(s);
                return;
            }
        }
        if (!s) {
            return;
        }
        size_t stateLen = 0;
        cleanerName(zfree, Bytes) uint8_t *state = ops->snapshot(s, &stateLen);
        ops->free(s);
        if (!state) {
            return;
        }

        for (size_t t = 0; t < e->len; t++) {
            pcDamagedOp(row, ops, state, stateLen, e, e->data, t, "truncated",
                        t);
        }
        if (e->len == 0) {
            continue;
        }
        cleanerName(zfree, Bytes) uint8_t *body = zmalloc(e->len);
        memcpy(body, e->data, e->len);
        const size_t positions = e->len <= 8 ? e->len : 8;
        for (size_t p = 0; p < positions; p++) {
            const size_t at =
                e->len <= 8 ? p : (size_t)(splitmix64(&rng) % e->len);
            const uint8_t was = body[at];
            if (was == 0) {
                continue; /* no bit to clear */
            }
            const uint8_t high =
                (uint8_t)(1u << (stdc_bit_width((unsigned)was) - 1));
            const uint8_t cleared[3] = {(uint8_t)(was & (was - 1)),
                                        (uint8_t)(was & ~high), 0};
            for (size_t m = 0; m < countof(cleared); m++) {
                if (m > 0 && cleared[m] == cleared[m - 1]) {
                    continue; /* a one-bit byte: the same corruption */
                }
                body[at] = cleared[m];
                pcDamagedOp(row, ops, state, stateLen, e, body, e->len,
                            "bits cleared", at);
            }
            body[at] = was;
        }
    }
}

/* Numeral-looking strings (contract 4) */
static const char *const pcNumerals[] = {
    "22",
    "007",
    "1.5",
    "-3",
    "0",
    "-0",
    "1e3",
    "18446744073709551615",
    "18446744073709551616",
    "-9223372036854775809",
    "170141183460469231731687303715884105727",
    "-170141183460469231731687303715884105728",
    "340282366920938463463374607431768211455",
};
#define PC_NUMERALS (countof(pcNumerals))

static void pcHolds(pcRow *row, const persistContractType *t, const void *s,
                    const databox *strings, const char *what) {
    for (size_t i = 0; i < PC_NUMERALS; i++) {
        if (!t->holdsBytes(s, &strings[i])) {
            PC_FAIL(row, t->ops, "%s: \"%s\" no longer stored as bytes", what,
                    pcNumerals[i]);
        }
    }
}

/* One build: contracts 1 and 2 (and 4 when `strings`), returning the
 * live snapshot for contract 3 when `snapOut`. */
static void pcCase(pcRow *row, const persistContractType *t, uint64_t seed,
                   size_t size, const databox *strings, uint8_t **snapOut,
                   size_t *snapOutLen) {
    const persistOps *ops = t->ops;
    snprintf(row->at, sizeof(row->at), "seed %" PRIu64 " size %zu%s", seed,
             size, strings ? " numerals" : "");
    persistContractLog log = {.ops = ops};
    const persistContractBuild b = {.seed = seed,
                                    .size = size,
                                    .strings = strings,
                                    .stringCount = strings ? PC_NUMERALS : 0,
                                    .log = &log};
    void *live = t->build(&b);
    if (!live) {
        PC_FAIL(row, ops, "%s", "build returned NULL");
        pcLogFree(&log);
        return;
    }

    uint8_t *snap = nullptr;
    size_t snapLen = 0;
    if (pcRoundTrip(row, ops, live, "round trip", &snap, &snapLen)) {
        /* The restored structure has the settings the live one was built
         * with (a setting the snapshot does not carry leaves both
         * snapshots equal, so only a direct comparison sees it) */
        if (t->sameSettings) {
            void *restored = pcRestore(ops, snap, snapLen);
            if (restored && !t->sameSettings(live, restored)) {
                PC_FAIL(row, ops, "%s", "restore changed a setting");
            }
            if (restored) {
                ops->free(restored);
            }
        }

        /* A snapshot-only type (no encodeOp/applyOp) has no WAL */
        const bool hasWal = ops->encodeOp && ops->applyOp;
        if (hasWal) {
            pcWal(row, ops, &log, snap, snapLen);
            /* The damage builds (snapOut) damage the log too */
            if (snapOut) {
                pcDamageOps(row, ops, &log, seed);
            }
        }
        if (strings) {
            pcHolds(row, t, live, strings, "live");
            void *r = pcRestore(ops, snap, snapLen);
            if (r) {
                pcHolds(row, t, r, strings, "restored");
                ops->free(r);
            }
            void *w = hasWal && log.start
                          ? pcRestore(ops, log.start, log.startLen)
                          : nullptr;
            if (w &&
                pcReplay(row, ops, &w, &log, 0, log.count, "numeral replay")) {
                pcHolds(row, t, w, strings, "replayed");
            }
            if (w) {
                ops->free(w);
            }
        }
    }

    /* Timing and size of the largest build */
    if (size >= row->elements && snap) {
        row->elements = ops->count(live);
        row->snapshotLen = snapLen;
        uint64_t t0 = timeUtilMonotonicNs();
        size_t len = 0;
        uint8_t *again = ops->snapshot(live, &len);
        uint64_t t1 = timeUtilMonotonicNs();
        void *r = ops->restore(snap, snapLen);
        uint64_t t2 = timeUtilMonotonicNs();
        row->snapshotNs = t1 - t0;
        row->restoreNs = t2 - t1;
        zfree(again);
        if (r) {
            ops->free(r);
        }
    }

    ops->free(live);
    pcLogFree(&log);
    if (snapOut) {
        *snapOut = snap;
        *snapOutLen = snapLen;
    } else {
        zfree(snap);
    }
}

static pcRow pcRun(const persistContractType *t) {
    static constexpr size_t sizes[] = {0, 1, 2, 37, 600, 6000};
    pcRow row = {};
    for (size_t i = 0; i < countof(sizes); i++) {
        for (uint64_t seed = 1; seed <= 3; seed++) {
            pcCase(&row, t, seed * 1000 + i, sizes[i], nullptr, nullptr,
                   nullptr);
        }
    }

    /* Damage the snapshots of small builds */
    for (uint64_t seed = 1; seed <= 2; seed++) {
        uint8_t *snap = nullptr;
        size_t snapLen = 0;
        pcCase(&row, t, seed, 37, nullptr, &snap, &snapLen);
        if (snap) {
            pcDamage(&row, t->ops, snap, snapLen, seed);
            zfree(snap);
        }
    }

    if (t->holdsBytes) {
        databox strings[PC_NUMERALS];
        for (size_t i = 0; i < PC_NUMERALS; i++) {
            strings[i] = DATABOX_WITH_BYTES((uint8_t *)pcNumerals[i],
                                            strlen(pcNumerals[i]));
        }
        pcCase(&row, t, 7, 0, strings, nullptr, nullptr);
        pcCase(&row, t, 8, 600, strings, nullptr, nullptr);
    }
    return row;
}

static double pcPer(uint64_t total, size_t n) {
    return n ? (double)total / (double)n : 0.0;
}

/* Every registered type, or with PERSIST_CONTRACT_ONLY=name in the
 * environment, just that one (extra datakit-test arguments name more
 * tests, so the choice is not an argument) */
int persistContractTest(int, char *[]) {
    const char *only = getenv("PERSIST_CONTRACT_ONLY");
    int err = 0;

    /* Every type and op code names itself uniquely (an unassigned code is
     * "unknown"), so files and tools never print one name for two. */
    for (int kind = 0; kind < 2; kind++) {
        const char *seen[256] = {};
        for (unsigned v = 0; v < 256; v++) {
            const char *name = kind == 0 ? persistTypeName((persistType)v)
                                         : persistOpName((persistOp)v);
            if (name == nullptr || name[0] == '\0') {
                ERR("%s %u has no name", kind == 0 ? "type" : "op", v);
                continue;
            }
            seen[v] = name;
            const bool unassigned =
                strcmp(name, "unknown") == 0 || strcmp(name, "UNKNOWN") == 0;
            for (unsigned w = 0; w < v && !unassigned; w++) {
                if (seen[w] && strcmp(seen[w], name) == 0) {
                    ERR("%s codes %u and %u are both \"%s\"",
                        kind == 0 ? "type" : "op", w, v, name);
                }
            }
        }
    }

    /* The persist registry (persistRegistry.c) and this list name the
     * same descriptors, one contract type each, and a file's type id
     * leads back to its own descriptor */
    {
        const persistOps *const *registered = nullptr;
        const size_t types = persistRegistry(&registered);
        if (types != countof(pcRegistry)) {
            ERR("the persist registry has %zu types, the contract %zu", types,
                countof(pcRegistry));
        }
        for (size_t i = 0; i < types; i++) {
            size_t found = 0;
            for (size_t j = 0; j < countof(pcRegistry); j++) {
                found += pcRegistry[j]->ops == registered[i];
            }
            if (found != 1) {
                ERR("%s: %zu contract types", registered[i]->name, found);
            }
            if (persistOpsForType(registered[i]->type) != registered[i]) {
                ERR("%s: its type id leads to another descriptor",
                    registered[i]->name);
            }
        }
    }

    printf("| type | elements | snapshot B/elem | snapshot ns/elem | "
           "restore ns/elem | WAL ops replayed | damaged refused | "
           "damaged accepted | damaged ops refused | damaged ops accepted | "
           "result |\n");
    printf("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- "
           "|\n");
    for (size_t i = 0; i < countof(pcRegistry); i++) {
        const persistContractType *t = pcRegistry[i];
        if (only && strcmp(only, t->ops->name) != 0) {
            continue;
        }
        /* The type's registered name is the one its files and tools
         * print for its type code. */
        if (strcmp(persistTypeName(t->ops->type), t->ops->name) != 0) {
            ERR("%s: persistTypeName(%u) is \"%s\"", t->ops->name,
                (unsigned)t->ops->type, persistTypeName(t->ops->type));
        }
        const pcRow row = pcRun(t);
        printf("| %s | %zu | %.2f | %.1f | %.1f | %zu | %zu | %zu | %zu | %zu "
               "| %s |\n",
               t->ops->name, row.elements, pcPer(row.snapshotLen, row.elements),
               pcPer(row.snapshotNs, row.elements),
               pcPer(row.restoreNs, row.elements), row.opsReplayed,
               row.damagedRefused, row.damagedAccepted, row.damagedOpsRefused,
               row.damagedOpsAccepted, row.failures ? "FAIL" : "pass");
        fflush(stdout);
        if (row.failures) {
            ERR("%s: %d contract failures", t->ops->name, row.failures);
        }
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
