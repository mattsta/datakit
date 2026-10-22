#include "multimapAtomDense.h"
#include "multiroar.h"

/* Invariant: the live ids are exactly [0, next) minus 'holes', and every
 * hole is below next - 1 (freeing the highest id lowers next instead). */
struct multimapAtomDense {
    multimapAtom *atoms;
    multiroar *holes; /* NULL while there are no holes */
    uint64_t next;    /* lowest id never handed out above every live id */
};

multimapAtomDense *multimapAtomDenseNew(void) {
    /* @own: caller; free=multimapAtomDenseFree */
    multimapAtomDense *d = zcalloc(1, sizeof(*d));
    /* @own: container; free=multimapAtomDenseFree */
    d->atoms = multimapAtomNew();
    return d;
}

void multimapAtomDenseFree(multimapAtomDense *d) {
    if (d) {
        multimapAtomFree(d->atoms);
        multiroarFree(d->holes);
        zfree(d);
    }
}

const multimapAtom *multimapAtomDenseBase(const multimapAtomDense *d) {
    return d->atoms;
}

/* The lowest id not in use, taken */
static uint64_t denseTakeId_(multimapAtomDense *d) {
    uint64_t id;
    if (d->holes && multiroarMin(d->holes, &id)) {
        multiroarRemove(d->holes, id);
        if (multiroarIsEmpty(d->holes)) {
            multiroarFree(d->holes);
            d->holes = NULL;
        }

        return id;
    }

    assert(d->next + 1 < MULTIMAP_ATOM_ID_LIMIT);
    return d->next++;
}

/* 'id' is no longer in use */
static void denseGiveBack_(multimapAtomDense *d, const uint64_t id) {
    if (id + 1 != d->next) {
        if (!d->holes) {
            /* @own: container; free=multimapAtomDenseFree */
            d->holes = multiroarBitNew();
        }

        multiroarBitSet(d->holes, id);
        return;
    }

    /* The highest id: lower next past any holes directly below it */
    d->next--;
    while (d->holes && d->next > 0 && multiroarBitGet(d->holes, d->next - 1)) {
        multiroarRemove(d->holes, d->next - 1);
        d->next--;
    }

    if (d->holes && multiroarIsEmpty(d->holes)) {
        multiroarFree(d->holes);
        d->holes = NULL;
    }
}

static databox denseRef_(const uint64_t id) {
    return (databox){.data.u = id,
                     .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
}

void multimapAtomDenseInsertConvert(multimapAtomDense *d, databox *box) {
    const uint64_t id = denseTakeId_(d);
    multimapAtomInsertWithExactAtomID(d->atoms, id, box);
    *box = denseRef_(id);
}

static bool denseInsertIfNew_(multimapAtomDense *d, databox *key,
                              const bool retain) {
    databox ref = {{0}};
    if (multimapAtomLookupReference(d->atoms, key, &ref)) {
        if (retain) {
            multimapAtomRetainByRef(d->atoms, &ref);
        }

        *key = denseRef_(ref.data.u);
        key->created = false;
        return true;
    }

    multimapAtomDenseInsertConvert(d, key);
    key->created = true;
    return false;
}

bool multimapAtomDenseInsertIfNewConvert(multimapAtomDense *d, databox *key) {
    return denseInsertIfNew_(d, key, false);
}

bool multimapAtomDenseInsertIfNewConvertAndRetain(multimapAtomDense *d,
                                                  databox *key) {
    return denseInsertIfNew_(d, key, true);
}

void multimapAtomDenseRetain(multimapAtomDense *d, const databox *key) {
    databox ref = {{0}};
    if (multimapAtomLookupReference(d->atoms, key, &ref)) {
        multimapAtomRetainByRef(d->atoms, &ref);
        return;
    }

    databox copy = *key;
    multimapAtomDenseInsertConvert(d, &copy);
}

void multimapAtomDenseRetainById(multimapAtomDense *d, const uint64_t id) {
    multimapAtomRetainById(d->atoms, id);
}

bool multimapAtomDenseReleaseById(multimapAtomDense *d, const uint64_t id) {
    const databox ref = denseRef_(id);
    databox count = {{0}};
    multimapAtomLookupRefcount(d->atoms, &ref, &count);
    if (count.type == DATABOX_VOID) {
        return false; /* not in the table */
    }

    if (!multimapAtomReleaseById(d->atoms, &ref)) {
        return false;
    }

    denseGiveBack_(d, id);
    return true;
}

bool multimapAtomDenseRelease(multimapAtomDense *d, const databox *key) {
    databox ref = {{0}};
    if (!multimapAtomLookupReference(d->atoms, key, &ref)) {
        return false;
    }

    return multimapAtomDenseReleaseById(d, ref.data.u);
}

bool multimapAtomDenseDeleteById(multimapAtomDense *d, const uint64_t id) {
    if (!multimapAtomDeleteById(d->atoms, id)) {
        return false;
    }

    denseGiveBack_(d, id);
    return true;
}

bool multimapAtomDenseDelete(multimapAtomDense *d, const databox *key) {
    databox ref = {{0}};
    if (!multimapAtomLookupReference(d->atoms, key, &ref)) {
        return false;
    }

    return multimapAtomDenseDeleteById(d, ref.data.u);
}

uint64_t multimapAtomDenseNextId(const multimapAtomDense *d) {
    uint64_t id;
    if (d->holes && multiroarMin(d->holes, &id)) {
        return id;
    }

    return d->next;
}

size_t multimapAtomDenseCount(const multimapAtomDense *d) {
    return multimapAtomCount(d->atoms);
}

size_t multimapAtomDenseBytes(const multimapAtomDense *d) {
    return sizeof(*d) + multimapAtomBytes(d->atoms) +
           (d->holes ? (size_t)multiroarMemoryUsage(d->holes) : 0);
}

#ifdef DATAKIT_TEST
#include "ctest.h"
#include "multimap.h" /* multimapTestAllocCountBegin/End */
#include <inttypes.h>

static uint64_t denseTestRand_(uint64_t *s) {
    uint64_t z = (*s += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

enum { DENSE_TEST_IDS_ = 2048 };

/* The oracle: which ids are live, the value each holds and its
 * references */
typedef struct denseOracle_ {
    bool live[DENSE_TEST_IDS_];
    int64_t value[DENSE_TEST_IDS_];
    uint32_t refs[DENSE_TEST_IDS_];
    size_t count;
} denseOracle_;

static uint64_t denseOracleLowestFree_(const denseOracle_ *o) {
    for (uint64_t id = 0; id < DENSE_TEST_IDS_; id++) {
        if (!o->live[id]) {
            return id;
        }
    }

    return DENSE_TEST_IDS_;
}

/* A random live id, or DENSE_TEST_IDS_ when none */
static uint64_t denseOraclePick_(const denseOracle_ *o, uint64_t *rng) {
    if (o->count == 0) {
        return DENSE_TEST_IDS_;
    }

    uint64_t id = denseTestRand_(rng) % DENSE_TEST_IDS_;
    while (!o->live[id]) {
        id = (id + 1) % DENSE_TEST_IDS_;
    }

    return id;
}

/* Values alternate between integers and strings so both kinds of key are
 * interned; the string form spells the integer with a prefix */
static databox denseValue_(const int64_t v, char *buf, const size_t cap) {
    if (v % 3 == 0) {
        const int len = snprintf(buf, cap, "v%" PRId64, v);
        return databoxNewBytes(buf, (size_t)len);
    }

    return databoxNewSigned(v);
}

static bool denseValueIs_(const databox *got, const int64_t v) {
    char buf[32];
    const databox want = denseValue_(v, buf, sizeof(buf));
    return databoxCompare(got, &want) == 0;
}

static int denseSoak_(const uint64_t seed, const size_t ops) {
    int err = 0;
    uint64_t rng = seed;
    static denseOracle_ o;
    memset(&o, 0, sizeof(o));
    int64_t nextValue = 1;
    multimapAtomDense *d = multimapAtomDenseNew();
    for (size_t op = 0; op < ops && !err; op++) {
        const uint64_t r = denseTestRand_(&rng) % 100;
        const uint64_t pick = denseOraclePick_(&o, &rng);
        char buf[32];
        if (r < 40 || pick == DENSE_TEST_IDS_) {
            /* A new value gets the lowest free id */
            if (o.count == DENSE_TEST_IDS_ - 1) {
                continue;
            }

            const uint64_t want = denseOracleLowestFree_(&o);
            const int64_t v = nextValue++;
            databox key = denseValue_(v, buf, sizeof(buf));
            if (multimapAtomDenseInsertIfNewConvert(d, &key) || !key.created ||
                key.data.u != want) {
                ERR("op %zu: new value got id %" PRIu64 ", want %" PRIu64, op,
                    (uint64_t)key.data.u, want);
                break;
            }

            o.live[want] = true;
            o.value[want] = v;
            o.refs[want] = 1;
            o.count++;
        } else if (r < 50) {
            /* An existing value: its id, no new reference */
            databox key = denseValue_(o.value[pick], buf, sizeof(buf));
            if (!multimapAtomDenseInsertIfNewConvert(d, &key) || key.created ||
                key.data.u != pick) {
                ERR("op %zu: existing value did not return id %" PRIu64, op,
                    pick);
            }
        } else if (r < 60) {
            databox key = denseValue_(o.value[pick], buf, sizeof(buf));
            multimapAtomDenseInsertIfNewConvertAndRetain(d, &key);
            o.refs[pick]++;
        } else if (r < 70) {
            multimapAtomDenseRetainById(d, pick);
            o.refs[pick]++;
        } else if (r < 95) {
            /* Release by id, or by value one time in five */
            const bool byValue = r >= 90;
            const databox key = denseValue_(o.value[pick], buf, sizeof(buf));
            const bool deleted = byValue
                                     ? multimapAtomDenseRelease(d, &key)
                                     : multimapAtomDenseReleaseById(d, pick);
            o.refs[pick]--;
            if (deleted != (o.refs[pick] == 0)) {
                ERR("op %zu: release of id %" PRIu64 " reported %s", op, pick,
                    deleted ? "a delete" : "no delete");
            }

            if (o.refs[pick] == 0) {
                o.live[pick] = false;
                o.count--;
            }
        } else {
            if (!multimapAtomDenseDeleteById(d, pick)) {
                ERR("op %zu: DeleteById(%" PRIu64 ") failed", op, pick);
            }

            o.live[pick] = false;
            o.refs[pick] = 0;
            o.count--;
        }

        if (multimapAtomDenseNextId(d) != denseOracleLowestFree_(&o) ||
            multimapAtomDenseCount(d) != o.count) {
            ERR("op %zu: next id %" PRIu64 " / count %zu, want %" PRIu64
                " / %zu",
                op, multimapAtomDenseNextId(d), multimapAtomDenseCount(d),
                denseOracleLowestFree_(&o), o.count);
        }

        if (op % 512 == 511) {
            for (uint64_t id = 0; id < DENSE_TEST_IDS_; id++) {
                if (!o.live[id]) {
                    continue;
                }

                const databox ref = denseRef_(id);
                databox got = {{0}};
                if (!multimapAtomLookup(multimapAtomDenseBase(d), &ref, &got) ||
                    !denseValueIs_(&got, o.value[id])) {
                    ERR("op %zu: id %" PRIu64 " does not resolve", op, id);
                    break;
                }
            }
        }
    }

    /* Delete everything: the free set goes, ids restart at 0 */
    for (uint64_t id = 0; id < DENSE_TEST_IDS_; id++) {
        if (o.live[id] && !multimapAtomDenseDeleteById(d, id)) {
            ERR("drain: DeleteById(%" PRIu64 ") failed", id);
        }
    }

    if (multimapAtomDenseCount(d) != 0 || multimapAtomDenseNextId(d) != 0 ||
        d->holes || d->next != 0) {
        ERRR("a drained table did not return to id 0 with no holes");
    }

    multimapAtomDenseFree(d);
    return err;
}

int multimapAtomDenseTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("lowest free id first, holes filled lowest first") {
        multimapAtomDense *d = multimapAtomDenseNew();
        databox v[8];
        for (int i = 0; i < 8; i++) {
            v[i] = databoxNewSigned(500 + i);
            multimapAtomDenseInsertConvert(d, &v[i]); /* ids 0..7 */
        }

        /* Free 5, 2 and 6: the next atoms take 2, 5, 6, then 8 */
        multimapAtomDenseReleaseById(d, 5);
        multimapAtomDenseReleaseById(d, 2);
        multimapAtomDenseDeleteById(d, 6);
        const uint64_t want[] = {2, 5, 6, 8};
        for (size_t i = 0; i < COUNT_ARRAY(want); i++) {
            databox x = databoxNewSigned(600 + (int64_t)i);
            multimapAtomDenseInsertConvert(d, &x);
            if (x.data.u != want[i]) {
                ERR("atom %zu got id %" PRIu64 ", want %" PRIu64, i,
                    (uint64_t)x.data.u, want[i]);
            }
        }

        /* Freeing the top ids lowers the next id past holes below them */
        multimapAtomDenseDeleteById(d, 6);
        multimapAtomDenseDeleteById(d, 8);
        multimapAtomDenseDeleteById(d, 7);
        if (multimapAtomDenseNextId(d) != 6 || d->next != 6 || d->holes) {
            ERR("next id %" PRIu64 " (counter %" PRIu64 "), want 6, no holes",
                multimapAtomDenseNextId(d), d->next);
        }

        multimapAtomDenseFree(d);
    }

    TEST("a missing value or id releases and deletes nothing") {
        multimapAtomDense *d = multimapAtomDenseNew();
        databox a = databoxNewSigned(1);
        multimapAtomDenseInsertConvert(d, &a);
        const databox missing = databoxNewSigned(2);
        if (multimapAtomDenseRelease(d, &missing) ||
            multimapAtomDenseReleaseById(d, 9) ||
            multimapAtomDenseDelete(d, &missing) ||
            multimapAtomDenseDeleteById(d, 9) ||
            multimapAtomDenseCount(d) != 1 || multimapAtomDenseNextId(d) != 1) {
            ERRR("a miss changed the table");
        }

        multimapAtomDenseFree(d);
    }

    TEST("Retain interns a new value and counts one more use of a known one") {
        multimapAtomDense *d = multimapAtomDenseNew();
        const databox apple = databoxNewBytesString("apple");
        multimapAtomDenseRetain(d, &apple); /* interned: one reference */
        multimapAtomDenseRetain(d, &apple); /* two */
        multimapAtomDenseRetain(d, &apple); /* three */
        if (multimapAtomDenseCount(d) != 1) {
            ERR("%zu atoms after retaining one value",
                multimapAtomDenseCount(d));
        }
        /* Two releases leave it; the third deletes it */
        if (multimapAtomDenseRelease(d, &apple) ||
            multimapAtomDenseRelease(d, &apple) ||
            !multimapAtomDenseRelease(d, &apple) ||
            multimapAtomDenseCount(d) != 0) {
            ERRR("three retains did not take three releases");
        }
        multimapAtomDenseFree(d);
    }

    TEST("differential soak against an oracle, allocations balanced") {
        multimapTestAllocCountBegin();
        for (uint64_t seed = 1; seed <= 3; seed++) {
            err += denseSoak_(seed, 6000);
        }

        const int64_t live = multimapTestAllocCountEnd();
        if (live != 0) {
            ERR("%" PRId64 " allocations outlived the soak", live);
        }
    }

    TEST("holes cost bytes only while they exist") {
        multimapAtomDense *d = multimapAtomDenseNew();
        for (int i = 0; i < 1000; i++) {
            databox x = databoxNewSigned(i);
            multimapAtomDenseInsertConvert(d, &x);
        }

        const size_t full = multimapAtomDenseBytes(d);
        if (d->holes) {
            ERRR("a table without holes keeps a free set");
        }

        for (uint64_t id = 0; id < 1000; id += 2) {
            multimapAtomDenseDeleteById(d, id);
        }

        if (!d->holes || multimapAtomDenseNextId(d) != 0) {
            ERRR("500 holes are not tracked");
        }

        for (int i = 0; i < 500; i++) {
            databox x = databoxNewSigned(5000 + i);
            multimapAtomDenseInsertConvert(d, &x);
        }

        if (d->holes || multimapAtomDenseNextId(d) != 1000) {
            ERRR("refilling every hole did not release the free set");
        }

        printf("    1000 atoms: %zu bytes; free set released when empty\n",
               full);
        multimapAtomDenseFree(d);
    }

    TEST_FINAL_RESULT;
}
#endif
