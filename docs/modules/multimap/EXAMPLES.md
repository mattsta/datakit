# multimap Worked Examples

## Overview

These examples focus on mechanics that are specific to multimap: in-place row
mutation through `multimapEntry`, the difference between map / set / full-width
inserts, range scans anchored at a key, reference-keyed maps, and the
sorted-prefix trim.

For cross-module recipes that combine multimap with other datakit containers,
see [docs/examples/USE_CASES.md](../../examples/USE_CASES.md) and
[docs/examples/PATTERNS.md](../../examples/PATTERNS.md).

Every snippet assumes:

```c
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "multimap.h"
```

Two rules apply throughout:

- **Byte values read out of a multimap are borrowed.** `databoxCBytes()`
  points into the flex until the next mutation. Copy before you mutate.
- **Never write through `*me.map` with a raw flex call.** Use
  `multimapReplaceEntry()` / `multimapResizeEntry()` so the cached middle offset
  (and, on Full, the submap rangeBox) is refreshed.

## Example 1: In-Place Row Mutation

A session table demonstrating `multimapGetUnderlyingEntry()` +
`multimapReplaceEntry()` (rewrite one column) and `multimapFieldIncr()`
(bump a counter column) without rewriting the whole row.

```c
/* Row layout: sessionId | username | lastAccess | hitCount */
enum { SESSION_COLUMNS = 4 };

enum sessionColumn {
    SESSION_COL_ID = 0,
    SESSION_COL_USERNAME = 1,
    SESSION_COL_LAST_ACCESS = 2,
    SESSION_COL_HITS = 3
};

typedef struct {
    multimap *sessions;
} SessionStore;

SessionStore *sessionStoreNew(void) {
    SessionStore *store = zcalloc(1, sizeof(*store));
    store->sessions = multimapNew(SESSION_COLUMNS);
    return store;
}

void sessionStoreFree(SessionStore *store) {
    if (store) {
        multimapFree(store->sessions);
        zfree(store);
    }
}

void sessionStoreAdd(SessionStore *store, uint64_t sessionId,
                     const char *username, int64_t now) {
    databox sid = databoxNewUnsigned(sessionId);
    databox user = databoxNewBytesString(username);
    databox access = databoxNewSigned(now);
    databox hits = databoxNewSigned(0);

    const databox *elements[SESSION_COLUMNS] = {&sid, &user, &access, &hits};

    /* Map mode: re-adding an existing sessionId replaces columns 1..3 */
    multimapInsert(&store->sessions, elements);
}

/* Rewrite ONE column in place: walk from the key to the target column,
 * then replace through the multimap wrapper. */
bool sessionStoreTouch(SessionStore *store, uint64_t sessionId, int64_t now) {
    databox sid = databoxNewUnsigned(sessionId);
    multimapEntry me;

    if (!multimapGetUnderlyingEntry(store->sessions, &sid, &me)) {
        return false;
    }

    /* me.fe starts on the key; step forward to lastAccess */
    for (uint32_t i = 0; i < SESSION_COL_LAST_ACCESS; i++) {
        me.fe = flexNext(*me.map, me.fe);
    }

    databox newTime = databoxNewSigned(now);
    multimapReplaceEntry(&store->sessions, &me, &newTime);

    /* Counter columns do not need the manual walk at all */
    multimapFieldIncr(&store->sessions, &sid, SESSION_COL_HITS, 1);
    return true;
}

/* Read a row out, copying the one byte-typed column we keep. */
bool sessionStoreGet(SessionStore *store, uint64_t sessionId,
                     char **outUsername, int64_t *outLastAccess,
                     int64_t *outHits) {
    databox sid = databoxNewUnsigned(sessionId);
    databox values[SESSION_COLUMNS - 1]; /* Lookup skips the key */

    if (!multimapLookupInto(store->sessions, &sid, values,
                            SESSION_COLUMNS - 1)) {
        return false;
    }

    /* values[0] borrows flex memory: copy it before any mutation */
    *outUsername = strndup((const char *)databoxCBytes(&values[0]),
                           (size_t)databoxLen(&values[0]));
    *outLastAccess = values[1].data.i64;
    *outHits = values[2].data.i64;
    return true;
}

/* Collect-then-delete. The keys are unsigned integers, which databox stores
 * immediately, so the collected boxes stay valid after the first delete.
 * A byte-typed key would have to be copied here instead. */
size_t sessionStoreExpireOlderThan(SessionStore *store, int64_t cutoff,
                                   size_t maxToExpire) {
    databox *expired = zcalloc(maxToExpire, sizeof(*expired));
    size_t expiredCount = 0;

    multimapIterator iter;
    multimapIteratorInit(store->sessions, &iter, true);

    databox row[SESSION_COLUMNS];
    while (expiredCount < maxToExpire &&
           multimapIteratorNextInto(&iter, row, SESSION_COLUMNS)) {
        if (row[SESSION_COL_LAST_ACCESS].data.i64 < cutoff) {
            expired[expiredCount++] = row[SESSION_COL_ID];
        }
    }

    for (size_t i = 0; i < expiredCount; i++) {
        multimapDelete(&store->sessions, &expired[i]);
    }

    zfree(expired);
    return expiredCount;
}

void exampleSessionStore(int64_t now) {
    SessionStore *store = sessionStoreNew();

    sessionStoreAdd(store, 12345, "alice", now);
    sessionStoreAdd(store, 67890, "bob", now - 3600);

    sessionStoreTouch(store, 12345, now);

    char *username;
    int64_t lastAccess;
    int64_t hits;
    if (sessionStoreGet(store, 12345, &username, &lastAccess, &hits)) {
        printf("session 12345: %s, last %" PRId64 ", hits %" PRId64 "\n",
               username, lastAccess, hits);
        free(username);
    }

    const size_t gone = sessionStoreExpireOlderThan(store, now - 1800, 1000);
    printf("expired %zu sessions\n", gone);

    sessionStoreFree(store);
}
```

**What to take away**

- `me.fe` lands on the key; every other column is `flexNext()` steps away.
- `multimapFieldIncr()` does the same walk internally, so a numeric column never
  needs the manual version.
- `multimapFieldIncr()` on an absent key returns 0 and changes nothing.
- Mutating while iterating is unsafe: collect keys, finish the walk, then
  delete.

## Example 2: Time Series With Duplicate Timestamps

Two readings can share a timestamp, so the store must allow duplicate keys.
That means `multimapInsertFullWidth()` everywhere - mixing it with
`multimapInsert()` on the same container would silently replace one of the
duplicates.

```c
/* Row layout: timestamp | value | tag */
enum { TS_COLUMNS = 3 };

typedef struct {
    multimap *metrics;
    char *metricName;
} TimeSeries;

TimeSeries *timeSeriesNew(const char *name) {
    TimeSeries *ts = zcalloc(1, sizeof(*ts));
    /* Larger submaps: this map is append-heavy and scanned in ranges */
    ts->metrics = multimapNewLimit(TS_COLUMNS, FLEX_CAP_LEVEL_4096);
    ts->metricName = strdup(name);
    return ts;
}

void timeSeriesFree(TimeSeries *ts) {
    if (ts) {
        multimapFree(ts->metrics);
        free(ts->metricName);
        zfree(ts);
    }
}

void timeSeriesRecord(TimeSeries *ts, int64_t timestamp, double value,
                      const char *tag) {
    databox tsBox = databoxNewSigned(timestamp);
    databox valBox = databoxNewReal(value);
    databox tagBox = databoxNewBytesString(tag);

    const databox *elements[TS_COLUMNS] = {&tsBox, &valBox, &tagBox};

    /* Full width: (timestamp, value, tag) triples may repeat a timestamp */
    multimapInsertFullWidth(&ts->metrics, elements);
}

typedef struct {
    int64_t timestamp;
    double value;
    char *tag;
} Metric;

/* Range scan: seed the iterator at startTime, stop when keys pass endTime. */
size_t timeSeriesQuery(TimeSeries *ts, int64_t startTime, int64_t endTime,
                       Metric *out, size_t outMax) {
    databox start = databoxNewSigned(startTime);
    multimapIterator iter;

    /* false means "startTime sorts past every row" - nothing to scan */
    if (!multimapIteratorInitAt(ts->metrics, &iter, true, &start)) {
        return 0;
    }

    size_t count = 0;
    databox row[TS_COLUMNS];

    while (count < outMax &&
           multimapIteratorNextInto(&iter, row, TS_COLUMNS)) {
        const int64_t stamp = row[0].data.i64;
        if (stamp > endTime) {
            break; /* sorted, so nothing later can match */
        }

        out[count].timestamp = stamp;
        out[count].value = row[1].data.d64;
        out[count].tag = strndup((const char *)databoxCBytes(&row[2]),
                                 (size_t)databoxLen(&row[2]));
        count++;
    }

    return count;
}

double timeSeriesMean(const Metric *metrics, size_t count) {
    if (count == 0) {
        return 0.0;
    }

    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        sum += metrics[i].value;
    }

    return sum / (double)count;
}

void exampleTimeSeries(int64_t now) {
    TimeSeries *cpu = timeSeriesNew("cpu.usage");

    timeSeriesRecord(cpu, now, 45.2, "host=server1");
    timeSeriesRecord(cpu, now, 51.8, "host=server2"); /* same timestamp! */
    timeSeriesRecord(cpu, now + 60, 52.1, "host=server1");
    timeSeriesRecord(cpu, now + 120, 48.9, "host=server1");

    Metric window[1024];
    const size_t found = timeSeriesQuery(cpu, now, now + 3600, window, 1024);

    printf("%zu readings, mean %.2f%%\n", found, timeSeriesMean(window, found));

    for (size_t i = 0; i < found; i++) {
        printf("  %" PRId64 ": %.2f (%s)\n", window[i].timestamp,
               window[i].value, window[i].tag);
        free(window[i].tag);
    }

    timeSeriesFree(cpu);
}
```

**What to take away**

- `multimapIteratorInitAt()` seeds at the first key `>= startAt`, and returns
  `false` only when that lands past the end.
- The scan can stop early because rows are sorted; there is no "range end"
  parameter.
- A container is either a map (unique keys) or a full-width bag. Pick one and
  use it consistently.

## Example 3: Inverted Index Keyed by Atom References

`multimapAtom` interns each word once; the index then stores the small integer
reference as its key instead of repeating the string on every posting.

```c
#include <ctype.h>

#include "multimapAtom.h"

/* Row layout: wordRef | docId | position */
enum { INDEX_COLUMNS = 3 };

typedef struct {
    multimapAtom *atoms;
    multimap *index;
} InvertedIndex;

InvertedIndex *invertedIndexNew(void) {
    InvertedIndex *idx = zcalloc(1, sizeof(*idx));
    idx->atoms = multimapAtomNew();
    idx->index = multimapNew(INDEX_COLUMNS);
    return idx;
}

void invertedIndexFree(InvertedIndex *idx) {
    if (idx) {
        multimapFree(idx->index);
        multimapAtomFree(idx->atoms);
        zfree(idx);
    }
}

void invertedIndexAddDocument(InvertedIndex *idx, uint64_t docId,
                              const char *text) {
    char *textCopy = strdup(text);
    char *word = strtok(textCopy, " \t\n.,;:!?");
    uint64_t position = 0;

    while (word) {
        for (char *p = word; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }

        /* Rewrites wordBox in place into a reference databox.
         * Returns true when the word was already interned. */
        databox wordBox = databoxNewBytesString(word);
        multimapAtomInsertIfNewConvert(idx->atoms, &wordBox);

        databox docBox = databoxNewUnsigned(docId);
        databox posBox = databoxNewUnsigned(position);
        const databox *entry[INDEX_COLUMNS] = {&wordBox, &docBox, &posBox};

        /* One word appears in many documents at many positions, so the
         * index is a full-width bag, not a key->value map. */
        multimapInsertFullWidth(&idx->index, entry);

        position++;
        word = strtok(NULL, " \t\n.,;:!?");
    }

    free(textCopy);
}

typedef struct {
    uint64_t docId;
    uint64_t position;
} SearchResult;

size_t invertedIndexSearch(InvertedIndex *idx, const char *searchWord,
                           SearchResult *out, size_t outMax) {
    char *lower = strdup(searchWord);
    for (char *p = lower; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }

    databox wordBox = databoxNewBytesString(lower);
    databox wordRef;
    const bool known = multimapAtomLookupReference(idx->atoms, &wordBox,
                                                   &wordRef);
    free(lower);

    if (!known) {
        return 0;
    }

    multimapIterator iter;
    if (!multimapIteratorInitAt(idx->index, &iter, true, &wordRef)) {
        return 0;
    }

    size_t count = 0;
    databox row[INDEX_COLUMNS];

    while (count < outMax &&
           multimapIteratorNextInto(&iter, row, INDEX_COLUMNS)) {
        /* Postings for one word are contiguous; stop at the next word */
        if (row[0].data.u != wordRef.data.u) {
            break;
        }

        out[count].docId = row[1].data.u;
        out[count].position = row[2].data.u;
        count++;
    }

    return count;
}

void exampleInvertedIndex(void) {
    InvertedIndex *idx = invertedIndexNew();

    invertedIndexAddDocument(idx, 1,
                             "the quick brown fox jumps over the lazy dog");
    invertedIndexAddDocument(idx, 2, "the lazy cat sleeps all day");
    invertedIndexAddDocument(idx, 3, "quick reflexes help the fox");

    SearchResult hits[256];
    size_t count = invertedIndexSearch(idx, "lazy", hits, 256);

    printf("'lazy' found in %zu locations:\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("  document %" PRIu64 ", position %" PRIu64 "\n", hits[i].docId,
               hits[i].position);
    }

    invertedIndexFree(idx);
}
```

**What to take away**

- `multimapAtomInsertIfNewConvert()` converts the caller's box in place and
  returns `true` when the string was already interned.
- This index stores raw reference values as keys, so it sorts by **atom ID**
  (first-sighting order), not alphabetically. Building the index through
  `multimapInsertWithSurrogateKey()` with the atom container would sort by the
  underlying strings instead.
- Postings for one word are contiguous, which is why the scan can stop as soon
  as the key changes.

## Example 4: Leaderboard With Repeating Scores

Two players can tie, so score cannot be a unique key. The store is a full-width
bag keyed by `(score, playerId, playerName)`, read backwards for the top N.

```c
/* Row layout: score | playerId | playerName */
enum { LB_COLUMNS = 3 };

typedef struct {
    multimap *scores;
} Leaderboard;

Leaderboard *leaderboardNew(void) {
    Leaderboard *lb = zcalloc(1, sizeof(*lb));
    lb->scores = multimapNew(LB_COLUMNS);
    return lb;
}

void leaderboardFree(Leaderboard *lb) {
    if (lb) {
        multimapFree(lb->scores);
        zfree(lb);
    }
}

void leaderboardAddScore(Leaderboard *lb, int64_t score, uint64_t playerId,
                         const char *playerName) {
    databox scoreBox = databoxNewSigned(score);
    databox idBox = databoxNewUnsigned(playerId);
    databox nameBox = databoxNewBytesString(playerName);

    const databox *entry[LB_COLUMNS] = {&scoreBox, &idBox, &nameBox};
    multimapInsertFullWidth(&lb->scores, entry);
}

/* Moving a player means removing the exact old row, not "the row with this
 * score" - several players may share it. */
void leaderboardMoveScore(Leaderboard *lb, int64_t oldScore, int64_t newScore,
                          uint64_t playerId, const char *playerName) {
    databox oldScoreBox = databoxNewSigned(oldScore);
    databox idBox = databoxNewUnsigned(playerId);
    databox nameBox = databoxNewBytesString(playerName);

    const databox *oldEntry[LB_COLUMNS] = {&oldScoreBox, &idBox, &nameBox};
    multimapDeleteFullWidth(&lb->scores, oldEntry);

    leaderboardAddScore(lb, newScore, playerId, playerName);
}

typedef struct {
    int64_t score;
    uint64_t playerId;
    char *playerName;
} LeaderboardEntry;

size_t leaderboardTopN(Leaderboard *lb, LeaderboardEntry *out, size_t n) {
    multimapIterator iter;
    multimapIteratorInit(lb->scores, &iter, false); /* highest score first */

    size_t count = 0;
    databox row[LB_COLUMNS];

    while (count < n && multimapIteratorNextInto(&iter, row, LB_COLUMNS)) {
        out[count].score = row[0].data.i64;
        out[count].playerId = row[1].data.u;
        out[count].playerName = strndup((const char *)databoxCBytes(&row[2]),
                                        (size_t)databoxLen(&row[2]));
        count++;
    }

    return count;
}

/* Rank = 1 + number of strictly higher scores. */
uint64_t leaderboardRankOf(Leaderboard *lb, int64_t targetScore) {
    multimapIterator iter;
    multimapIteratorInit(lb->scores, &iter, false);

    uint64_t higher = 0;
    databox row[LB_COLUMNS];

    while (multimapIteratorNextInto(&iter, row, LB_COLUMNS)) {
        if (row[0].data.i64 <= targetScore) {
            break; /* reverse order: everything after this is lower */
        }

        higher++;
    }

    return higher + 1;
}

void exampleLeaderboard(void) {
    Leaderboard *lb = leaderboardNew();

    leaderboardAddScore(lb, 9500, 1, "Alice");
    leaderboardAddScore(lb, 8200, 2, "Bob");
    leaderboardAddScore(lb, 8200, 3, "Charlie"); /* tie: both rows kept */
    leaderboardAddScore(lb, 9100, 4, "Diana");

    LeaderboardEntry top[3];
    const size_t count = leaderboardTopN(lb, top, 3);

    printf("Top %zu players:\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("%zu. %s: %" PRId64 "\n", i + 1, top[i].playerName,
               top[i].score);
        free(top[i].playerName);
    }

    printf("Score 8200 is rank %" PRIu64 "\n", leaderboardRankOf(lb, 8200));

    leaderboardMoveScore(lb, 8200, 9800, 2, "Bob");

    leaderboardFree(lb);
}
```

**What to take away**

- Ties require `multimapInsertFullWidth()`; `multimapInsert()` would keep only
  one player per score.
- Reverse iteration still fills `row[0..N-1]` in column order, so the same array
  works in both directions.
- Removing one row of a duplicated key needs `multimapDeleteFullWidth()`;
  `multimapDelete()` targets whichever row that key finds.

## Example 5: Retention Trim and Key Set Algebra

`multimapDeleteByPredicate()` is a **sorted-prefix trim**: it drops everything
at or below a boundary in one operation instead of walking rows. Only
`MULTIMAP_CONDITION_LESS_THAN_EQUAL` is implemented; every other condition
asserts.

```c
/* Drop every event whose timestamp key is <= cutoff. */
bool eventsTrimOlderThan(multimap **events, int64_t cutoff) {
    databox boundary = databoxNewSigned(cutoff);
    const multimapPredicate trim = {
        .condition = MULTIMAP_CONDITION_LESS_THAN_EQUAL,
        .compareAgainst = boundary};

    return multimapDeleteByPredicate(events, &trim);
}

/* Counting walk. multimapProcessUntil STOPS at the first row whose key fails
 * the predicate, so it is a prefix walk, not a filter. Unlike the trim, it
 * accepts every multimapCondition. */
static bool countRow(void *userData, const databox *elements[]) {
    uint64_t *seen = userData;
    (void)elements;
    (*seen)++;
    return true; /* returning false stops the walk early */
}

uint64_t eventsCountUpTo(multimap *events, int64_t boundaryValue) {
    databox boundary = databoxNewSigned(boundaryValue);
    const multimapPredicate upTo = {
        .condition = MULTIMAP_CONDITION_LESS_THAN_EQUAL,
        .compareAgainst = boundary};

    uint64_t seen = 0;
    multimapProcessUntil(events, &upTo, true, countRow, &seen);
    return seen;
}

/* Key set algebra. All three helpers read element 0 of each row and insert
 * into 'dst' with multimapInsert(), so 'dst' is normally a 1-element map. */
void exampleKeySets(multimap *a, multimap *b) {
    multimap *both = multimapNew(1);
    multimapIterator ia;
    multimapIterator ib;

    multimapIteratorInit(a, &ia, true);
    multimapIteratorInit(b, &ib, true);
    multimapIntersectKeys(&both, &ia, &ib);

    /* multimapDifferenceKeys asserts BOTH iterators come from 1-element
     * multimaps, so project the keys first. */
    multimap *keysA = multimapNew(1);
    multimap *keysB = multimapNew(1);
    multimapCopyKeys(&keysA, a);
    multimapCopyKeys(&keysB, b);

    multimap *onlyInA = multimapNew(1);
    multimapIteratorInit(keysA, &ia, true);
    multimapIteratorInit(keysB, &ib, true);
    multimapDifferenceKeys(&onlyInA, &ia, &ib, false);

    multimap *symmetric = multimapNew(1);
    multimapIteratorInit(keysA, &ia, true);
    multimapIteratorInit(keysB, &ib, true);
    multimapDifferenceKeys(&symmetric, &ia, &ib, true);

    /* multimapCopyKeys is a union: repeated calls accumulate into dst */
    multimap *union_ = multimapNew(1);
    multimapCopyKeys(&union_, a);
    multimapCopyKeys(&union_, b);

    printf("intersection %zu, A-only %zu, symmetric %zu, union %zu\n",
           multimapCount(both), multimapCount(onlyInA),
           multimapCount(symmetric), multimapCount(union_));

    multimapFree(both);
    multimapFree(onlyInA);
    multimapFree(symmetric);
    multimapFree(union_);
    multimapFree(keysA);
    multimapFree(keysB);
}
```

**What to take away**

- The trim is the only bulk-delete path and it only understands
  `MULTIMAP_CONDITION_LESS_THAN_EQUAL`. Anything else must iterate.
- `multimapProcessUntil()` accepts all conditions but stops at the first
  failing row; use it for prefix work, not filtering.
- `multimapDifferenceKeys()` requires 1-element multimaps on both sides;
  `multimapIntersectKeys()` does not.
- Iterators are consumed by these calls: re-init before reusing one.

## Performance Notes

### Append only for non-decreasing keys

`multimapAppend()` skips the submap-selection step and inserts straight into the
highest submap. It is not O(1) - the insert inside that submap is still a sorted
insert - and on Small it is literally `multimapInsert()`. Appending a key that
sorts below an earlier submap puts the row in the wrong submap and corrupts
range routing.

```c
/* Fine: keys never decrease */
for (int64_t i = 0; i < 10000; i++) {
    databox k = databoxNewSigned(i);
    databox v = databoxNewSigned(i * 2);
    const databox *e[2] = {&k, &v};
    multimapAppend(&m, e);
}

/* Not fine: random keys must go through multimapInsert() */
```

### Match the size limit to the workload

The `flexCapSizeLimit` level decides when Small promotes, when Medium promotes,
and how large a Full submap may grow before splitting. Smaller submaps mean
smaller memmoves per write; larger submaps mean fewer indirections per read.

```c
multimap *hot = multimapNewLimit(2, FLEX_CAP_LEVEL_512);   /* write-heavy */
multimap *bulk = multimapNewLimit(2, FLEX_CAP_LEVEL_4096); /* read-heavy */
```

### Prefer the `*Into` readers

`multimapLookupInto()`, `multimapFirstInto()`, `multimapLastInto()` and
`multimapIteratorNextInto()` build the pointer array for you and compile to the
same stores as a hand-rolled initializer. They cap at `MULTIMAP_INTO_MAX` (16)
boxes; wider rows must use the raw pointer-array API.

Remember the widths differ: `multimapLookupInto()` takes
`elementsPerEntry - 1`, everything else takes `elementsPerEntry`. These are not
partial reads, so an undersized array is written past its end.

### Intern repeated strings

When the same byte string repeats across many rows, intern it once and store the
reference:

```c
multimapAtom *atoms = multimapAtomNew();
multimap *logs = multimapNew(3); /* timestamp | levelRef | message */

databox level = databoxNewBytesString("ERROR");
multimapAtomInsertIfNewConvert(atoms, &level);
/* 'level' is now a small integer reference */
```

Use `multimapInsertWithSurrogateKey()` and the `*WithReference` lookups when the
map must still sort by the underlying strings.

### Do not build an LRU on raw multimap

Scanning every row to find the oldest timestamp is O(n) per eviction. datakit
already ships an eviction-aware container; see
[MULTILRU.md](../multi/MULTILRU.md).

## See Also

- [MULTIMAP.md](MULTIMAP.md) - Complete API reference
- [VARIANTS.md](VARIANTS.md) - Small/Medium/Full internals and the atom API
- [databox](../core/DATABOX.md) - Value container
- [flex](../flex/FLEX.md) - Underlying sorted storage
- [docs/examples/USE_CASES.md](../../examples/USE_CASES.md) - Cross-module
  application recipes
