# Benchmarking datakit

How to measure datakit code so the numbers mean something: the build
configuration that makes measurement valid, the timing and reporting
helpers the platform already provides, and worked harnesses for the
containers people most often want to compare.

**This file does not publish numbers.** Recorded, machine-tagged,
dated results live in [PERFORMANCE_BASELINES.md](../PERFORMANCE_BASELINES.md),
which is the only place performance claims should be quoted from.

## Table of Contents

1. [Build Configuration](#build-configuration)
2. [Running the Built-In Benchmarks](#running-the-built-in-benchmarks)
3. [A Timing Harness](#a-timing-harness)
4. [Measuring Memory](#measuring-memory)
5. [Integer Set Benchmarks](#integer-set-benchmarks)
6. [Map Benchmarks](#map-benchmarks)
7. [List Benchmarks](#list-benchmarks)
8. [Compression Benchmarks](#compression-benchmarks)
9. [Cardinality Benchmarks](#cardinality-benchmarks)
10. [Reporting Results](#reporting-results)

---

## Build Configuration

Benchmarks are only valid from the `perf` preset: Release `-O3`,
`-mcpu=native`, LTO, and **no sanitizers**. ASAN/UBSAN add 3-10×
overhead and invalidate every number you collect under them.

```bash
cmake --preset perf
cmake --build build-perf
```

Rules that make a capture comparable to an earlier one:

- Same preset, recorded alongside the numbers.
- Record CPU model, architecture, compiler, and date.
- Warm up before timing: the first pass through a fresh container pays
  page faults and allocator growth that steady-state work does not.
- Report a median (or minimum) of repeated runs, never a single run.
- Never benchmark a debug build, and never compare across presets.

---

## Running the Built-In Benchmarks

The test binary exposes benchmarks through a verb grammar. With no
arguments it prints usage; `list` enumerates everything runnable.

```bash
# Discover what exists
./build-perf/src/datakit-test list

# String buffer benchmarks
./build-perf/src/datakit-test bench mdsbench
./build-perf/src/datakit-test bench mdscbench

# Whole-system throughput sweep (default 64 MB working set)
./build-perf/src/datakit-test speed
./build-perf/src/datakit-test speed 128
./build-perf/src/datakit-test speed --json
./build-perf/src/datakit-test speed --csv

# Row-level regression gate against a locally kept baseline TSV
./build-perf/src/datakit-test test perfRegression
```

Two benchmarks build as standalone executables rather than test-binary
subcommands:

```bash
cmake --build build-perf --target intsetSearchBench memboundBench
./build-perf/intsetSearchBench
./build-perf/memboundBench
MEMBOUND_BENCH_MEMSHARD=1 ./build-perf/memboundBench
```

Run one named test rather than the whole suite: `datakit-test test ALL`
is a long full-suite run and is not a benchmark.

---

## A Timing Harness

Use `timeUtil` rather than hand-rolling `gettimeofday`. The
**monotonic** clocks are the ones to subtract — wall clocks step when
the system clock is adjusted, which silently corrupts a long capture.

```c
#include "timeUtil.h"

#include <inttypes.h>
#include <stdio.h>

typedef struct benchResult {
    const char *name;
    uint64_t elapsedNs;
    uint64_t operations;
    size_t bytes;
} benchResult;

static void benchReport(const benchResult *r) {
    const double seconds = (double)r->elapsedNs / 1e9;
    const double perOp = (double)r->elapsedNs / (double)r->operations;

    printf("%-28s %9.3f ms  %10.1f ns/op  %12.0f ops/s",
           r->name, (double)r->elapsedNs / 1e6, perOp,
           (double)r->operations / seconds);

    if (r->bytes) {
        printf("  %8.2f B/entry",
               (double)r->bytes / (double)r->operations);
    }

    printf("\n");
}

/* Median of N repeats: one run tells you about the scheduler, not
 * about your code. */
static uint64_t benchMedianNs(uint64_t (*run)(void *), void *userData,
                              size_t repeats) {
    uint64_t samples[16];
    if (repeats > 16) {
        repeats = 16;
    }

    for (size_t i = 0; i < repeats; i++) {
        samples[i] = run(userData);
    }

    /* Insertion sort: repeats is tiny by construction. */
    for (size_t i = 1; i < repeats; i++) {
        const uint64_t v = samples[i];
        size_t j = i;
        while (j > 0 && samples[j - 1] > v) {
            samples[j] = samples[j - 1];
            j--;
        }
        samples[j] = v;
    }

    return samples[repeats / 2];
}
```

Each `run` function is just a timed block:

```c
static uint64_t runSomething(void *userData) {
    workload *w = userData;

    const uint64_t start = timeUtilMonotonicNs();
    workloadExecute(w);
    return timeUtilMonotonicNs() - start;
}
```

### Generating Input Data

Use the `str.h` generators. They are fast, seeded, and reproducible, so
a rerun measures your change rather than a different data set.

```c
#include "str.h"

static void seedRng(uint64_t s[2], uint64_t seed) {
    s[0] = splitmix64(&seed);
    s[1] = splitmix64(&seed);
}

uint64_t rng[2];
seedRng(rng, 0x9e3779b97f4a7c15ULL);

for (size_t i = 0; i < count; i++) {
    values[i] = (int64_t)(xoroshiro128plus(rng) % universeSize);
}
```

---

## Measuring Memory

Every datakit container reports its own footprint. Prefer these to
process RSS: RSS includes allocator slack, other subsystems, and
whatever the OS has not reclaimed yet.

```c
size_t intsetBytes(intset *is);
size_t multimapBytes(const multimap *m);
size_t multilistBytes(const multilist *m);
size_t multilruBytes(const multilru *mlru);
size_t multimapAtomBytes(const multimapAtom *ma);
uint64_t multidictBytes(multidict *d);
uint64_t multiroarMemoryUsage(const multiroar *r);
size_t mflexBytesUncompressed(const mflex *m);
size_t mflexBytesActual(const mflex *m);
```

For arena-scoped work, `membound` reports its own usage directly —
`memboundBytesUsed`, `memboundCapacity`, `memboundGetFragmentation` —
which is far more precise than sampling RSS around the workload.

---

## Integer Set Benchmarks

`intset` is a sorted, width-adaptive array: 2, 4, or 8 bytes per element
depending on the widest value present, with branchless binary search for
lookups. The two things worth measuring are **bytes per element** across
encodings and **lookup cost** against the obvious alternatives.

```c
#include "intset.h"
#include "str.h"
#include "timeUtil.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

enum { INTSET_N = 100000 };

/* Encoding width drives the memory story: build three sets whose
 * contents differ only in the widest value present. */
static void benchIntsetEncodings(void) {
    const int64_t forcedWidth[3] = {0, INT64_C(100000), INT64_C(10000000000)};
    const char *label[3] = {"int16", "int32", "int64"};

    for (size_t w = 0; w < 3; w++) {
        intset *is = intsetNew();
        if (forcedWidth[w]) {
            intsetAdd(&is, forcedWidth[w], NULL);
        }

        const uint64_t start = timeUtilMonotonicNs();
        for (int64_t i = 0; i < 10000; i++) {
            intsetAdd(&is, i, NULL);
        }
        const uint64_t elapsedNs = timeUtilMonotonicNs() - start;

        printf("%-6s %8.3f ms  %8zu bytes  %.2f B/element\n", label[w],
               (double)elapsedNs / 1e6, intsetBytes(is),
               (double)intsetBytes(is) / (double)intsetCount(is));

        intsetFree(is);
    }
}

/* Lookup: intset binary search versus a linear scan over a plain
 * array of the same contents. Probe with values drawn at random so
 * neither side benefits from a predictable access pattern. */
static void benchIntsetLookup(void) {
    intset *is = intsetNew();
    int64_t *arr = zcalloc(INTSET_N, sizeof(*arr));

    for (int64_t i = 0; i < INTSET_N; i++) {
        intsetAdd(&is, i, NULL);
        arr[i] = i;
    }

    uint64_t rng[2];
    uint64_t seed = 1;
    rng[0] = splitmix64(&seed);
    rng[1] = splitmix64(&seed);

    int64_t *probes = zcalloc(INTSET_N, sizeof(*probes));
    for (size_t i = 0; i < INTSET_N; i++) {
        probes[i] = (int64_t)(xoroshiro128plus(rng) % INTSET_N);
    }

    uint64_t found = 0;
    uint64_t start = timeUtilMonotonicNs();
    for (size_t i = 0; i < INTSET_N; i++) {
        found += intsetFind(is, probes[i]);
    }
    const uint64_t setNs = timeUtilMonotonicNs() - start;

    start = timeUtilMonotonicNs();
    for (size_t i = 0; i < INTSET_N; i++) {
        for (size_t j = 0; j < INTSET_N; j++) {
            if (arr[j] == probes[i]) {
                found++;
                break;
            }
        }
    }
    const uint64_t scanNs = timeUtilMonotonicNs() - start;

    printf("intset find   %8.3f ms (%.1f ns/op)\n", (double)setNs / 1e6,
           (double)setNs / INTSET_N);
    printf("linear scan   %8.3f ms (%.1f ns/op)\n", (double)scanNs / 1e6,
           (double)scanNs / INTSET_N);
    printf("checksum      %" PRIu64 "\n", found);  /* defeats DCE */

    intsetFree(is);
    zfree(arr);
    zfree(probes);
}
```

The `checksum` print is not decoration: without consuming the result,
`-O3` is free to delete the loop you meant to time. Every benchmark in
this file consumes its output for that reason.

`intsetSearchBench` is the maintained version of the lookup benchmark
and is what the recorded baselines come from.

---

## Map Benchmarks

`multimap` keeps entries sorted, so it competes on ordered iteration and
range scans, not on raw point-lookup throughput against a hash table.
Measure both so the trade is visible.

```c
#include "multimap.h"
#include "timeUtil.h"

#include <inttypes.h>
#include <stdio.h>

enum { MAP_N = 50000 };

static void benchMultimap(void) {
    multimap *m = multimapNew(2);

    char key[32];
    char value[64];

    uint64_t start = timeUtilMonotonicNs();
    for (uint64_t i = 0; i < MAP_N; i++) {
        const size_t keyLen =
            (size_t)snprintf(key, sizeof(key), "key_%" PRIu64, i);
        const size_t valueLen =
            (size_t)snprintf(value, sizeof(value), "value_%" PRIu64, i);

        const databox keyBox = databoxNewBytes(key, keyLen);
        const databox valueBox = databoxNewBytes(value, valueLen);
        const databox *entry[2] = {&keyBox, &valueBox};

        multimapInsert(&m, entry);
    }
    const uint64_t insertNs = timeUtilMonotonicNs() - start;

    start = timeUtilMonotonicNs();
    uint64_t hits = 0;
    for (uint64_t i = 0; i < MAP_N; i++) {
        const size_t keyLen =
            (size_t)snprintf(key, sizeof(key), "key_%" PRIu64, i);
        const databox keyBox = databoxNewBytes(key, keyLen);

        databox found;
        hits += multimapLookupInto(m, &keyBox, &found, 1);
    }
    const uint64_t lookupNs = timeUtilMonotonicNs() - start;

    /* Ordered full scan: this is what the sorted layout buys. */
    start = timeUtilMonotonicNs();
    multimapIterator iter;
    multimapIteratorInit(m, &iter, true);

    uint64_t scanned = 0;
    databox entry[2];
    while (multimapIteratorNextInto(&iter, entry, 2)) {
        scanned += databoxLen(&entry[1]);
    }
    const uint64_t scanNs = timeUtilMonotonicNs() - start;

    printf("insert   %8.3f ms  %6.1f ns/op\n", (double)insertNs / 1e6,
           (double)insertNs / MAP_N);
    printf("lookup   %8.3f ms  %6.1f ns/op  (%" PRIu64 " hits)\n",
           (double)lookupNs / 1e6, (double)lookupNs / MAP_N, hits);
    printf("scan     %8.3f ms  %6.1f ns/entry (%" PRIu64 " bytes)\n",
           (double)scanNs / 1e6, (double)scanNs / MAP_N, scanned);
    printf("memory   %8zu bytes  %.2f B/entry\n", multimapBytes(m),
           (double)multimapBytes(m) / (double)multimapCount(m));

    multimapFree(m);
}
```

### Comparing Against multidict

`multidict` is the unsorted hash table. A fair comparison runs the same
key set through both and reports insert, lookup, and memory for each:

```c
#include "multidict.h"

multidict *d = multidictNew(&multidictTypeExactKey,
                            multidictDefaultClassNew(), 0);

/* ... same workload, then: */
printf("multidict %" PRIu64 " entries, %" PRIu64 " bytes\n",
       multidictCount(d), multidictBytes(d));

multidictFree(d);
```

Report the ordered-scan number alongside: a hash table has no equivalent
operation, and that asymmetry is the whole reason both exist.

### Variant Sweep

`multimap` promotes itself Small → Medium → Full as it grows. To see
where each representation stops paying, pin the variant and sweep sizes:

```c
static void benchMultimapVariants(void) {
    const size_t sizes[] = {100, 1000, 10000, 100000};

    for (size_t s = 0; s < 4; s++) {
        const size_t n = sizes[s];

        /* Pinned Small: no promotion, no tag dispatch. */
        multimapSmall *small = multimapSmallNew(2, false);
        uint64_t start = timeUtilMonotonicNs();
        for (int64_t i = 0; i < (int64_t)n; i++) {
            const databox k = databoxNewSigned(i);
            const databox v = databoxNewSigned(i * 2);
            const databox *e[2] = {&k, &v};
            multimapSmallInsert(small, e);
        }
        const uint64_t smallNs = timeUtilMonotonicNs() - start;
        const size_t smallBytes = multimapSmallBytes(small);

        /* Generic handle: promotes as needed. */
        multimap *full = multimapNew(2);
        start = timeUtilMonotonicNs();
        for (int64_t i = 0; i < (int64_t)n; i++) {
            const databox k = databoxNewSigned(i);
            const databox v = databoxNewSigned(i * 2);
            const databox *e[2] = {&k, &v};
            multimapInsert(&full, e);
        }
        const uint64_t genericNs = timeUtilMonotonicNs() - start;

        printf("%7zu  small %9.3f ms / %8zu B   generic %9.3f ms / %8zu B\n",
               n, (double)smallNs / 1e6, smallBytes,
               (double)genericNs / 1e6, multimapBytes(full));

        multimapSmallFree(small);
        multimapFree(full);
    }
}
```

Pinned Small stays a single sorted `flex`: excellent while it is small,
quadratic in insert cost once it is not. The crossover is the number
this sweep exists to find on your hardware, for your key shape.

---

## List Benchmarks

```c
#include "multilist.h"
#include "timeUtil.h"

#include <inttypes.h>
#include <stdio.h>

enum { LIST_N = 100000 };

static void benchMultilist(void) {
    multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);

    mflexState *s0 = mflexStateCreate();
    mflexState *s1 = mflexStateCreate();
    mflexState *s[2] = {s0, s1};

    uint64_t start = timeUtilMonotonicNs();
    for (int64_t i = 0; i < LIST_N; i++) {
        const databox item = databoxNewSigned(i);
        multilistPushByTypeTail(&ml, s0, &item);
    }
    const uint64_t appendNs = timeUtilMonotonicNs() - start;

    start = timeUtilMonotonicNs();
    multilistIterator iter;
    multilistIteratorInitForwardReadOnly(ml, s, &iter);

    multilistEntry entry;
    int64_t sum = 0;
    while (multilistNext(&iter, &entry)) {
        sum += entry.box.data.i64;
    }
    multilistIteratorRelease(&iter);
    const uint64_t iterNs = timeUtilMonotonicNs() - start;

    printf("append   %8.3f ms  %6.1f ns/op\n", (double)appendNs / 1e6,
           (double)appendNs / LIST_N);
    printf("iterate  %8.3f ms  %6.1f ns/entry (sum %" PRId64 ")\n",
           (double)iterNs / 1e6, (double)iterNs / LIST_N, sum);
    printf("memory   %8zu bytes  %.2f B/entry\n", multilistBytes(ml),
           (double)multilistBytes(ml) / (double)multilistCount(ml));

    multilistFree(ml);
    mflexStateFree(s0);
    mflexStateFree(s1);
}
```

Two knobs change the answer, so sweep them rather than guessing:

- **Node size** (`FLEX_CAP_LEVEL_*`): larger nodes mean fewer nodes and
  better scan locality, but costlier mid-list insert.
- **Uncompressed depth** (second `multilistNew` argument): `0` compresses
  everything eligible; `N` keeps the outermost `N` nodes at each end
  uncompressed so head/tail work never decompresses.

Comparing against a hand-rolled linked list is a memory comparison more
than a time one: measure `multilistBytes(ml)` against
`LIST_N * sizeof(YourNode)` and report both figures.

---

## Compression Benchmarks

Compression benchmarks are only meaningful against realistic input. The
useful experiment is the **same harness over inputs of different
entropy**, because that spread is the result.

```c
#include "mflex.h"
#include "str.h" /* splitmix64 / xoroshiro128plus */
#include "timeUtil.h"

#include <stdio.h>

static void benchMflex(const char *label, size_t count,
                       void (*fill)(char *buf, size_t bufLen, size_t i,
                                    uint64_t rng[2], size_t *outLen)) {
    mflexState *state = mflexStateCreate();
    mflex *m = mflexNew();

    uint64_t seed = 42;
    uint64_t rng[2] = {splitmix64(&seed), splitmix64(&seed)};

    char buf[128];
    const uint64_t start = timeUtilMonotonicNs();
    for (size_t i = 0; i < count; i++) {
        size_t len = 0;
        fill(buf, sizeof(buf), i, rng, &len);
        mflexPushBytes(&m, state, buf, len, FLEX_ENDPOINT_TAIL);
    }
    const uint64_t elapsedNs = timeUtilMonotonicNs() - start;

    const size_t actual = mflexBytesActual(m);
    const size_t uncompressed = mflexBytesUncompressed(m);

    printf("%-16s %8.3f ms  actual %9zu B", label,
           (double)elapsedNs / 1e6, actual);

    if (mflexIsCompressed(m)) {
        printf("  ratio %.2fx", (double)uncompressed / (double)actual);
    } else {
        printf("  (not compressed)");
    }

    printf("\n");

    mflexFree(m);
    mflexStateFree(state);
}

static void fillRepetitive(char *buf, size_t bufLen, size_t i,
                           uint64_t rng[2], size_t *outLen) {
    (void)i;
    (void)rng;
    *outLen = (size_t)snprintf(buf, bufLen,
                               "repetitive log line, status: OK");
}

static void fillRandom(char *buf, size_t bufLen, size_t i, uint64_t rng[2],
                       size_t *outLen) {
    (void)i;
    const size_t len = 63 < bufLen ? 63 : bufLen - 1;
    for (size_t j = 0; j < len; j++) {
        buf[j] = (char)('a' + (xoroshiro128plus(rng) % 26));
    }
    *outLen = len;
}
```

Run both and report the pair. High-entropy input compressing to slightly
_more_ than its input is the expected, correct outcome — that overhead is
the reason `mflex` decides per node whether compression is worth it, and
why `mflexIsCompressed` is worth checking before quoting a ratio.

### Delta-of-Delta and XOR Streams

`dod` and `xof` are bit-packed streams rather than containers: you own
the buffer, so "compressed size" is `(usedBits + 7) / 8`, and the
baseline is `count * sizeof(int64_t)` or `count * sizeof(double)`.

```c
#include "dod.h"
#include "timeUtil.h"

enum { DOD_N = 100000 };

static void benchDod(void) {
    dod *bits = zcalloc(DOD_N * 2, sizeof(*bits));

    const int64_t base = 1700000000;
    dodVal t0 = base;
    dodVal t1 = base + 60;
    size_t usedBits = 0;

    const uint64_t start = timeUtilMonotonicNs();
    for (size_t i = 2; i < DOD_N; i++) {
        const dodVal next = base + (int64_t)i * 60;
        dodAppend(bits, t0, t1, next, &usedBits);
        t0 = t1;
        t1 = next;
    }
    const uint64_t elapsedNs = timeUtilMonotonicNs() - start;

    const size_t encoded = (usedBits + 7) / 8;
    const size_t raw = DOD_N * sizeof(int64_t);

    printf("dod encode  %8.3f ms  %8zu B vs %8zu B raw  (%.2fx, %.2f bits/value)\n",
           (double)elapsedNs / 1e6, encoded, raw,
           (double)raw / (double)encoded, (double)usedBits / DOD_N);

    zfree(bits);
}
```

Perfectly regular intervals are the best case and produce a number you
cannot reproduce on real data. Always run the same harness over a real
timestamp sample before quoting a ratio. `xof` follows the same shape
with `xofInit` / `xofAppend`; see
[PATTERNS.md](PATTERNS.md#compression).

---

## Cardinality Benchmarks

`hyperloglog` trades exactness for a fixed footprint. Measure the error,
not just the speed — a cardinality estimator whose error you have not
measured is not a benchmark result.

```c
#include "hyperloglog.h"
#include "timeUtil.h"

#include <inttypes.h>
#include <math.h>

static void benchHyperloglog(uint64_t unique) {
    hyperloglog *hll = hyperloglogNew();

    const uint64_t start = timeUtilMonotonicNs();
    for (uint64_t i = 0; i < unique; i++) {
        /* hyperloglogAdd takes ** : a sparse HLL is reallocated and
         * eventually promoted to dense as it fills. */
        hyperloglogAdd(&hll, &i, sizeof(i));
    }
    const uint64_t elapsedNs = timeUtilMonotonicNs() - start;

    bool invalid = false;
    const uint64_t estimate = hyperloglogCount(hll, &invalid);

    const double error =
        100.0 * fabs((double)estimate - (double)unique) / (double)unique;

    printf("hll add     %8.3f ms  %6.1f ns/op\n", (double)elapsedNs / 1e6,
           (double)elapsedNs / (double)unique);
    printf("estimate    %" PRIu64 " vs %" PRIu64 " actual  (%.2f%% error)%s\n",
           estimate, unique, error, invalid ? " [corrupt]" : "");

    hyperloglogFree(hll);
}
```

Sweep `unique` across several orders of magnitude. A sparse HLL is small
and exact-ish at low cardinality and promotes to a fixed-size dense
encoding as it fills, so a single data point hides the whole behaviour.

For an exact-membership comparison, run the same key set through
`multiroar` (`multiroarBitSet` / `multiroarBitCount` /
`multiroarMemoryUsage`) and report both footprints. They answer
different questions — approximate count versus exact membership — so
report what each one can do, not just how big it is.

---

## Reporting Results

A capture is worth keeping only if someone else can reproduce it. Record:

- Date, CPU model and architecture, compiler and version.
- Preset (`perf`) and the exact build and run commands.
- Repeat count and whether you report median or minimum.
- The workload shape: element count, key size, value size, data
  distribution and RNG seed.
- Both time and memory. A container that is 10% slower and 60% smaller
  is a win in most of the places datakit gets used, and a table with
  only one column cannot show that.

Append new captures to
[PERFORMANCE_BASELINES.md](../PERFORMANCE_BASELINES.md) as a new dated
section; never rewrite an old one. The point of that file is the time
series. Row-level regressions are gated by `datakit-test test
perfRegression` against a locally kept TSV.

Next:

- [PATTERNS.md](PATTERNS.md) — the idioms these harnesses use
- [USE_CASES.md](USE_CASES.md) — the applications worth measuring
- [PERFORMANCE_BASELINES.md](../PERFORMANCE_BASELINES.md) — recorded numbers
