#include "intsetU32.h"
#include "cleaner.h"
#include "datakit.h"
#include "dkRandom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct intsetU32 {
    uint32_t count;
    uint32_t capacity; /* slots allocated in 'contents' (>= count) */
    uint32_t *contents;
};

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(intsetU32 *, intsetU32Free);

/* Return the value at pos, using the configured encoding. */
DK_INLINE_ALWAYS uint32_t intsetU32Get_(const intsetU32 *const is, size_t pos) {
    return is->contents[pos];
}

uint32_t *intsetU32Array(intsetU32 *is) {
    return is->contents;
}

/* The element buffer is a SEPARATE allocation reached through the handle;
 * an empty set carries a NULL buffer.  See intsetU32.h for the stable-handle
 * invariant these helpers uphold. */

/* Set the value at pos, using the configured encoding. */
DK_INLINE_ALWAYS void intsetU32Set_(intsetU32 *is, size_t pos,
                                    uint_fast32_t value) {
    is->contents[pos] = value;
}

/* Create an empty intsetU32. */
intsetU32 *intsetU32New(void) {
    /* @own: caller; free=intsetU32Free */
    intsetU32 *is = zcalloc(1, sizeof(*is));
    return is;
}

intsetU32 *intsetU32NewLen(const uint32_t len) {
    /* @own: caller; free=intsetU32Free */
    intsetU32 *is = zcalloc(1, sizeof(*is));
    if (len) {
        /* @own: container; free=zfree */
        is->contents = zcalloc(len, sizeof(uint32_t));
        is->capacity = len;
    }
    return is;
}

void intsetU32Free(intsetU32 *is) {
    if (is) {
        zfree(is->contents);
        zfree(is);
    }
}

intsetU32 *intsetU32Copy(const intsetU32 *is) {
    /* @own: caller; free=intsetU32Free */
    intsetU32 *result = zcalloc(1, sizeof(*result));
    result->count = is->count;
    if (is->count) {
        const size_t bufBytes = sizeof(uint32_t) * is->count;
        /* @own: container; free=zfree */
        result->contents = zmalloc(bufBytes);
        memcpy(result->contents, is->contents, bufBytes);
        result->capacity = is->count;
    }
    return result;
}

/* Resize the element buffer to hold exactly 'len' slots.  Reallocates the
 * buffer behind the stable handle — the handle pointer itself never
 * changes.  'len' == 0 releases the buffer (empty set carries a NULL
 * buffer). */
void intsetU32Resize(intsetU32 *is, const uint32_t len) {
    if (is->count > len) {
        is->count = len; /* members sort: the smallest 'len' stay */
    }
    if (len == 0) {
        zfree(is->contents);
        is->contents = nullptr;
        is->capacity = 0;
        return;
    }
    /* @own: container; free=zfree */
    is->contents = zrealloc(is->contents, sizeof(uint32_t) * len);
    is->capacity = len;
}

/* Room for 'need' slots: grows by half again (plus a few), so a run of
 * adds reallocates O(log n) times instead of once per add */
static void intsetU32Reserve_(intsetU32 *is, const uint32_t need) {
    if (need <= is->capacity) {
        return;
    }
    uint64_t cap = (uint64_t)is->capacity + is->capacity / 2 + 4;
    cap = cap < need ? need : cap;
    cap = cap > UINT32_MAX ? UINT32_MAX : cap;
    intsetU32Resize(is, (uint32_t)cap);
}

void intsetU32ShrinkToSize(intsetU32 *is) {
    intsetU32Resize(is, is->count);
}

void intsetU32UpdateCount(intsetU32 *is, const uint32_t len) {
    /* Past the room the array has, every read would run off its end */
    assert(len <= is->capacity);
    is->count = len;
}

/* Search for the position of "value".
 * Return true when the value was found.
 *   Sets "pos" to the position of the value within the intsetU32.
 * Return false when value is not present in the intsetU32
 *   Sets "pos" to the position where "value" can be inserted. */
static bool intsetU32Search(const intsetU32 *const is,
                            const uint_fast32_t value, uint32_t *pos) {
    /* The value can never be found when the set is empty */
    if (is->count == 0) {
        if (pos) {
            *pos = 0;
        }

        return false;
    }

    /* Check for the case where we know we cannot find the value,
     * but do know the insert position. */
    if (value > intsetU32Get_(is, is->count - 1)) {
        if (pos) {
            *pos = is->count;
        }

        return false;
    } else if (value < intsetU32Get_(is, 0)) {
        if (pos) {
            *pos = 0;
        }

        return false;
    }

    uint_fast32_t min = 0;
    uint_fast32_t max = is->count - 1;

    while (max >= min) {
        uint_fast32_t mid = (min + max) >> 1;
        uint_fast32_t cur = intsetU32Get_(is, mid);
        if (value > cur) {
            min = mid + 1;
        } else if (value < cur) {
            max = mid - 1;
        } else {
            if (pos) {
                *pos = mid;
            }

            return true;
        }
    }

    if (pos) {
        *pos = min;
    }

    return false;
}

static void intsetU32MoveTail(intsetU32 *is, uint32_t from, uint32_t to) {
    void *src, *dst;
    uint_fast32_t bytes = is->count - from;

    src = is->contents + from;
    dst = is->contents + to;
    bytes *= sizeof(uint32_t);

    memmove(dst, src, bytes);
}

/* Insert an integer in the intsetU32 */
/* Returns 'true' if element is added as new.
 * Returns 'false' if element already existed. */
bool intsetU32Add(intsetU32 *is, uint32_t value) {
    uint32_t pos;

    /* Abort if the value is already present in the set.
     * This call will populate "pos" with the right position to insert
     * the value when it cannot be found. */
    if (intsetU32Search(is, value, &pos)) {
        return false;
    }

    intsetU32Reserve_(is, is->count + 1);
    if (pos < is->count) {
        intsetU32MoveTail(is, pos, pos + 1);
    }

    intsetU32Set_(is, pos, value);
    is->count++;
    return true;
}

/* Delete integer from intsetU32 */
bool intsetU32Remove(intsetU32 *is, uint32_t value) {
    uint32_t pos;

    if (intsetU32Search(is, value, &pos)) {
        uint32_t len = is->count;

        /* Overwrite value with tail and update count */
        if (pos < (len - 1)) {
            intsetU32MoveTail(is, pos + 1, pos);
        }

        /* Update count BEFORE shrinking: Resize(0) frees the buffer, and
         * MoveTail above already consumed the pre-shrink count.  The
         * buffer halves once it is a quarter full (and is freed when the
         * set empties), so a run of removes reallocates rarely too. */
        is->count = len - 1;
        if (is->count == 0) {
            intsetU32Resize(is, 0);
        } else if (is->count <= is->capacity / 4) {
            intsetU32Resize(is, is->capacity / 2);
        }

        return true;
    }

    return false;
}

/* Determine whether a value belongs to this set */
bool intsetU32Exists(const intsetU32 *const is, const uint32_t value) {
    return intsetU32Search(is, value, nullptr);
}

bool intsetU32Equal(const intsetU32 *const a, const intsetU32 *const b) {
    if (a == b) {
        return true;
    }

    if (a->count == b->count) {
        return memcmp(a->contents, b->contents,
                      sizeof(*a->contents) * a->count) == 0;
    }

    return false;
}

/* Check if 'a' is a subset of 'b': every member of a exists in b.
 * Argument order matters: every caller passes (source, target) asking
 * "does source fit inside target". */
bool intsetU32Subset(const intsetU32 *const a, const intsetU32 *const b) {
    if (a == b) {
        return true;
    }

    for (size_t i = 0; i < a->count; i++) {
        if (!intsetU32Exists(b, a->contents[i])) {
            return false;
        }
    }

    return true;
}

uint32_t intsetU32Merge(intsetU32 *dst, const intsetU32 *b) {
    uint32_t merged = 0;
    for (size_t i = 0; i < b->count; i++) {
        merged += intsetU32Add(dst, b->contents[i]);
    }

    return merged;
}

/* Return random member (0 for an empty set - check count first when 0
 * is a legitimate member) */
uint32_t intsetU32Random(intsetU32 *is) {
    if (is->count == 0) {
        return 0;
    }

    /* A uniform position from the per-thread generator */
    return intsetU32Get_(is, (uint32_t)dkRandomBounded(is->count));
}

bool intsetU32RandomDelete(intsetU32 *is, uint32_t *deleted) {
    if (is->count == 0) {
        return false;
    }

    *deleted = intsetU32Random(is);
    return intsetU32Remove(is, *deleted);
}

/* Get the value at the given position. When this position is
 * out of range the function returns false, when in range it returns true. */
bool intsetU32Get(const intsetU32 *is, uint32_t pos, uint32_t *value) {
    if (pos < is->count) {
        *value = intsetU32Get_(is, pos);
        return true;
    }

    return false;
}

/* Return intsetU32 count */
size_t intsetU32Count(const intsetU32 *is) {
    return is->count;
}

/* Return intsetU32 blob size in bytes. */
size_t intsetU32Bytes(const intsetU32 *is) {
    return sizeof(intsetU32) + (sizeof(uint32_t) * is->capacity);
}

#ifdef DATAKIT_TEST
void intsetU32Repr(const intsetU32 *is) {
    printf("[");
    for (uint32_t i = 0; i < is->count; i++) {
        printf("%u, ", intsetU32Get_(is, i));
    }

    printf("]\n");
}

#include "timeUtil.h"
#include <time.h>
static void ok(void) {
    printf("OK\n");
}

#define intsetU32Assert(_e)                                                    \
    ((_e) ? (void)0 : (_intsetU32Assert(#_e, __FILE__, __LINE__), abort()))
static void _intsetU32Assert(char *estr, char *file, int line) {
    printf("\n\n=== intsetU32AssertION FAILED ===\n");
    printf("==> %s:%d '%s' is not true\n", file, line, estr);
}

static intsetU32 *createSet(int bits, int size) {
    uint64_t mask = (1 << bits) - 1;
    uint64_t value;
    /* @own: caller; free=intsetU32Free */
    intsetU32 *is = intsetU32New();

    for (int i = 0; i < size; i++) {
        if (bits > 32) {
            value = (random() * random()) & mask;
        } else {
            value = random() & mask;
        }

        intsetU32Add(is, value);
    }

    return is;
}

/* ====================================================================
 * The model test: random sequences of every public call against a sorted
 * array, every read checked
 * ==================================================================== */
static constexpr uint32_t U32_MODEL_CAP = 4000;

typedef struct u32Model_ {
    uint32_t n;
    uint32_t v[U32_MODEL_CAP];
} u32Model_;

defcleanerName(u32Model_ *, zfree, U32Model);

static uint64_t u32ModelRng_;
static uint32_t u32ModelRand_(void) {
    return (uint32_t)dkSplitmix64Next(&u32ModelRng_);
}

static uint32_t u32ModelLower_(const u32Model_ *M, uint32_t x) {
    uint32_t lo = 0;
    uint32_t hi = M->n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (M->v[mid] < x) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static bool u32ModelHas_(const u32Model_ *M, uint32_t x) {
    const uint32_t at = u32ModelLower_(M, x);
    return at < M->n && M->v[at] == x;
}

static void u32ModelAdd_(u32Model_ *M, uint32_t x) {
    const uint32_t at = u32ModelLower_(M, x);
    if (at < M->n && M->v[at] == x) {
        return;
    }
    memmove(&M->v[at + 1], &M->v[at], (M->n - at) * sizeof(M->v[0]));
    M->v[at] = x;
    M->n++;
}

static void u32ModelRemove_(u32Model_ *M, uint32_t x) {
    const uint32_t at = u32ModelLower_(M, x);
    if (at < M->n && M->v[at] == x) {
        memmove(&M->v[at], &M->v[at + 1], (M->n - at - 1) * sizeof(M->v[0]));
        M->n--;
    }
}

/* @model: src/intsetU32.h */
/* The set holds exactly the model's members */
static bool u32ModelSame_(intsetU32 *is, const u32Model_ *M) {
    if (intsetU32Count(is) != M->n ||
        (M->n && intsetU32Bytes(is) < M->n * sizeof(uint32_t))) {
        return false;
    }
    const uint32_t *a = intsetU32Array(is);
    for (uint32_t i = 0; i < M->n; i++) {
        uint32_t got = 0;
        if (a[i] != M->v[i] || !intsetU32Get(is, i, &got) || got != M->v[i]) {
            return false;
        }
    }
    uint32_t past = 0;
    return !intsetU32Get(is, M->n, &past);
}

/* @model: src/intsetU32.h */
/* Returns the step that disagreed, or UINT32_MAX when every one agreed */
static uint32_t intsetU32TestModel_(uint64_t seed) {
    cleanerName(zfree, U32Model) u32Model_ *M = zcalloc(1, sizeof(*M));
    cleaner(intsetU32Free) intsetU32 *is = intsetU32New();
    u32ModelRng_ = seed;
    const uint32_t steps = 8000;
    for (uint32_t step = 0; step < steps; step++) {
        const uint32_t r = u32ModelRand_();
        const uint32_t x = u32ModelRand_() % 6000;
        const bool draining = step >= steps * 2 / 3;
        const uint32_t op = r % 16;
        bool ok = true;
        if (op < (draining ? 2u : 5u) && M->n < U32_MODEL_CAP - 64) {
            ok = intsetU32Add(is, x) == !u32ModelHas_(M, x);
            u32ModelAdd_(M, x);
        } else if (op < 8) {
            const uint32_t y = M->n && r % 2 ? M->v[(r >> 8) % M->n] : x;
            ok = intsetU32Remove(is, y) == u32ModelHas_(M, y);
            u32ModelRemove_(M, y);
        } else if (op == 8) {
            ok = intsetU32Exists(is, x) == u32ModelHas_(M, x);
        } else if (op == 9) { /* a random member, or one taken out */
            if (r % 2) {
                uint32_t gone = 0;
                const bool took = intsetU32RandomDelete(is, &gone);
                ok = took == (M->n > 0) && (!took || u32ModelHas_(M, gone));
                if (took) {
                    u32ModelRemove_(M, gone);
                }
            } else {
                ok = M->n == 0 || u32ModelHas_(M, intsetU32Random(is));
            }
        } else if (op == 10 && M->n + 40 < U32_MODEL_CAP) {
            /* a merge of a few others: the ones new to the set count */
            cleaner(intsetU32Free) intsetU32 *b = intsetU32New();
            uint32_t want = 0;
            for (uint32_t i = 0; i < 1 + r % 30; i++) {
                const uint32_t y = u32ModelRand_() % 6000;
                if (intsetU32Add(b, y)) {
                    want += !u32ModelHas_(M, y);
                    u32ModelAdd_(M, y);
                }
            }
            ok = intsetU32Merge(is, b) == want;
        } else if (op == 11) { /* a copy: equal, and each a subset */
            cleaner(intsetU32Free) intsetU32 *copy = intsetU32Copy(is);
            ok = intsetU32Equal(copy, is) && intsetU32Subset(is, copy) &&
                 intsetU32Subset(copy, is) && u32ModelSame_(copy, M);
            if (ok && M->n) { /* one fewer: still a subset, not equal */
                intsetU32Remove(copy, M->v[0]);
                ok = intsetU32Subset(copy, is) && !intsetU32Equal(copy, is) &&
                     !intsetU32Subset(is, copy);
            }
        } else if (op == 12 && M->n) { /* room changed, then fitted */
            if (r % 4 == 0) {          /* below the count: the smallest stay */
                const uint32_t keep = (r >> 8) % M->n;
                intsetU32Resize(is, keep);
                M->n = keep;
            } else {
                intsetU32Resize(is, M->n + (r >> 8) % 50);
            }
            intsetU32ShrinkToSize(is);
        } else if (op == 13) { /* built by hand: written, then counted */
            cleaner(intsetU32Free) intsetU32 *built = intsetU32NewLen(M->n);
            memcpy(intsetU32Array(built), M->v, M->n * sizeof(M->v[0]));
            intsetU32UpdateCount(built, M->n);
            ok = intsetU32Equal(built, is) && u32ModelSame_(built, M);
        }
        if (!ok || (step % 40 == 39 && !u32ModelSame_(is, M))) {
            printf("model step %u: op %u disagreed (%u members)\n",
                   (unsigned)step, (unsigned)op, (unsigned)M->n);
            return step;
        }
    }
    return u32ModelSame_(is, M) ? UINT32_MAX : steps;
}

int intsetU32Test(int, char **) {
    srandom(time(nullptr));

    printf("Model: every public call against a sorted array: ");
    for (uint64_t seed = 1; seed <= 4; seed++) {
        intsetU32Assert(intsetU32TestModel_(seed) == UINT32_MAX);
    }
    printf("SUCCESS\n");

    printf("Random picks are members; RandomDelete drains every member: ");
    {
        cleaner(intsetU32Free) intsetU32 *is = intsetU32New();
        uint32_t deleted = 0;
        intsetU32Assert(intsetU32Random(is) == 0);
        intsetU32Assert(!intsetU32RandomDelete(is, &deleted));
        for (uint32_t v = 0; v < 100; v++) {
            intsetU32Add(is, v * 3);
        }
        for (int i = 0; i < 200; i++) {
            intsetU32Assert(intsetU32Exists(is, intsetU32Random(is)));
        }
        bool seen[300] = {};
        for (uint32_t n = 100; n > 0; n--) {
            intsetU32Assert(intsetU32RandomDelete(is, &deleted));
            intsetU32Assert(deleted % 3 == 0 && deleted < 300 &&
                            !seen[deleted]);
            seen[deleted] = true;
            intsetU32Assert(!intsetU32Exists(is, deleted));
            intsetU32Assert(intsetU32Count(is) == n - 1);
        }
        intsetU32Assert(!intsetU32RandomDelete(is, &deleted));
        printf("SUCCESS\n");
    }

    printf("A resize below the count keeps the smallest members: ");
    {
        cleaner(intsetU32Free) intsetU32 *is = intsetU32New();
        for (uint32_t v = 0; v < 100; v++) {
            intsetU32Add(is, v * 2);
        }
        intsetU32Resize(is, 10);
        intsetU32Assert(intsetU32Count(is) == 10);
        intsetU32Assert(intsetU32Exists(is, 18) && !intsetU32Exists(is, 20));
        intsetU32Assert(intsetU32Add(is, 501) && intsetU32Count(is) == 11);
        intsetU32Resize(is, 0);
        intsetU32Assert(intsetU32Count(is) == 0 && !intsetU32Exists(is, 0));
        printf("SUCCESS\n");
    }

    printf("Basic adding: ");
    {
        cleaner(intsetU32Free) intsetU32 *is = intsetU32New();
        intsetU32Assert(is);
        const bool s1 = intsetU32Add(is, 5);
        intsetU32Assert(is);
        intsetU32Assert(s1);
        const bool s2 = intsetU32Add(is, 6);
        intsetU32Assert(is);
        intsetU32Assert(s2);
        const bool s3 = intsetU32Add(is, 4);
        intsetU32Assert(is);
        intsetU32Assert(s3);
        const bool s4 = intsetU32Add(is, 4);
        intsetU32Assert(is);
        intsetU32Assert(!s4);
        ok();
    }

    printf("Subset semantics (a subset of b, matching intsetBigSubset): ");
    {
        cleaner(intsetU32Free) intsetU32 *small = intsetU32New();
        cleaner(intsetU32Free) intsetU32 *big = intsetU32New();
        for (uint32_t v = 10; v <= 30; v += 10) {
            intsetU32Add(small, v); /* {10, 20, 30} */
        }

        for (uint32_t v = 10; v <= 50; v += 10) {
            intsetU32Add(big, v); /* {10, 20, 30, 40, 50} */
        }

        /* Subset(a, b) asks "is a contained in b" — argument order is
         * the contract shared with intsetBigSubset. */
        intsetU32Assert(intsetU32Subset(small, big));
        intsetU32Assert(!intsetU32Subset(big, small));

        /* Equal sets are subsets of each other; self is a subset. */
        cleaner(intsetU32Free) intsetU32 *dup = intsetU32Copy(small);
        intsetU32Assert(intsetU32Subset(small, dup));
        intsetU32Assert(intsetU32Subset(dup, small));
        intsetU32Assert(intsetU32Subset(small, small));

        /* The empty set is a subset of everything; nothing non-empty
         * is a subset of the empty set. */
        cleaner(intsetU32Free) intsetU32 *empty = intsetU32New();
        intsetU32Assert(intsetU32Subset(empty, small));
        intsetU32Assert(!intsetU32Subset(small, empty));

        /* Disjoint overlap: sharing SOME members is not enough. */
        cleaner(intsetU32Free) intsetU32 *shifted = intsetU32New();
        intsetU32Add(shifted, 20);
        intsetU32Add(shifted, 999);
        intsetU32Assert(!intsetU32Subset(shifted, big));

        ok();
    }

    printf("Large number of random adds: ");
    {
        uint32_t inserts = 0;
        cleaner(intsetU32Free) intsetU32 *is = intsetU32New();
        for (int i = 0; i < 1024; i++) {
            if (intsetU32Add(is, random() % 0x800)) {
                inserts++;
            }
        }

        intsetU32Assert(is->count == inserts);
        ok();
    }

    printf("Upgrade from int16 to int32: ");
    {
        {
            cleaner(intsetU32Free) intsetU32 *is = intsetU32New();
            intsetU32Add(is, 32);
            intsetU32Add(is, 65535);
            intsetU32Assert(intsetU32Exists(is, 32));
            intsetU32Assert(intsetU32Exists(is, 65535));
        }

        {
            cleaner(intsetU32Free) intsetU32 *is = intsetU32New();
            intsetU32Add(is, 32);
            intsetU32Add(is, -65535);
            intsetU32Assert(intsetU32Exists(is, 32));
            intsetU32Assert(intsetU32Exists(is, -65535));
        }

        ok();
    }

    printf("Stress lookups: ");
    {
        int32_t num = 100'000, size = 10000;
        int i, bits = 20;
        cleaner(intsetU32Free) intsetU32 *is = createSet(bits, size);

        const uint64_t start = timeUtilUs();
        for (i = 0; i < num; i++) {
            intsetU32Search(is, random() % ((1 << bits) - 1), nullptr);
        }
        printf("%" PRId32 " lookups, %" PRId32 " element set, %" PRIu64
               "usec\n",
               num, size, timeUtilUs() - start);
    }

    printf("Stress add+delete: ");
    {
        int i, v1, v2;
        cleaner(intsetU32Free) intsetU32 *is = intsetU32New();
        for (i = 0; i < 0xffff; i++) {
            v1 = random() % 0xfff;
            intsetU32Add(is, v1);
            intsetU32Assert(intsetU32Exists(is, v1));

            v2 = random() % 0xfff;
            intsetU32Remove(is, v2);
            intsetU32Assert(!intsetU32Exists(is, v2));
        }

        ok();
    }

    printf("Capacity grows ahead and shrinks back: ");
    {
        /* Adds grow the buffer geometrically; removes halve it once a
         * quarter full and free it when empty; Bytes reports what is
         * allocated, and the values stay intact throughout */
        cleaner(intsetU32Free) intsetU32 *is = intsetU32New();
        for (uint32_t v = 0; v < 10000; v++) {
            intsetU32Add(is, v);
        }
        intsetU32Assert(intsetU32Count(is) == 10000);
        intsetU32Assert(is->capacity >= 10000 && is->capacity < 15100);
        intsetU32Assert(intsetU32Bytes(is) ==
                        sizeof(intsetU32) + sizeof(uint32_t) * is->capacity);

        for (uint32_t v = 0; v < 9990; v++) {
            intsetU32Remove(is, v);
        }
        intsetU32Assert(intsetU32Count(is) == 10 && is->capacity < 64);
        for (uint32_t v = 9990; v < 10000; v++) {
            intsetU32Assert(intsetU32Exists(is, v));
        }

        for (uint32_t v = 9990; v < 10000; v++) {
            intsetU32Remove(is, v);
        }
        intsetU32Assert(intsetU32Count(is) == 0 && is->capacity == 0 &&
                        intsetU32Array(is) == nullptr);
        ok();
    }

    printf("Repr prints an empty and a filled set: ");
    {
        intsetU32 *is = intsetU32New();
        intsetU32Repr(is);
        for (uint32_t v = 0; v < 20; v += 3) {
            intsetU32Add(is, v);
        }
        intsetU32Repr(is);
        intsetU32Free(is);
        ok();
    }

    return 0;
}

#endif

/* This is modified and improved version of original malfunctions introduced by:
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
