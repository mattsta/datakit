# linearBloom and linearBloomCount: Bloom filters

**Headers**: `linearBloom.h`, `linearBloomCount.h` (header-only: every call is inline)
**Tests**: `linearBloom` (`bloom`)

Two fixed-size Bloom filters, each one allocation:

| Filter             | Each slot          | Answers                                                   | Default size                          | Default capacity at ~1 in 10,000 false positives |
| ------------------ | ------------------ | --------------------------------------------------------- | ------------------------------------- | ------------------------------------------------ |
| `linearBloom`      | 1 bit              | "possibly added" or "certainly not"                       | 2^23 bits (1 MB), 13 hashes           | about 430,000 items                              |
| `linearBloomCount` | 3-bit counter, 0-7 | about how many times a key was added (capped at 7), decay | 2,875,518 counters (~1 MB), 13 hashes | about 150,000 items                              |

Both take the key as **two 64-bit hashes** the caller computes, and derive the 13 probe positions
from them (Kirsch-Mitzenmacher double hashing; the second hash is forced odd so the probes never
collapse onto one slot). Hashing once per key and reusing the pair for every filter that sees the key
is the intended use.

```c
#include "linearBloom.h"
#include "rapidhash.h"

/* The pair for a key: two seeds give two independent hashes */
static void keyHash(const void *key, size_t len, uint64_t hash[2]) {
    hash[0] = rapidhashWithSeed(key, len, 0);
    hash[1] = rapidhashWithSeed(key, len, hash[0]);
}

static void bloomExample(void) {
    linearBloom *seen = linearBloomNew();
    uint64_t hash[2];
    keyHash("user:42", 7, hash);

    const bool before = linearBloomHashSet(seen, hash); /* false: new */
    const bool again = linearBloomHashCheck(seen, hash); /* true */
    (void)before;
    (void)again;

    /* The share of bits set: the false-positive rate is about
     * utilization^13, so past ~0.5 the filter is due to be replaced */
    const double full = linearBloomUtilization(seen);
    (void)full;
    linearBloomFree(seen);
}
```

## linearBloom

- `linearBloomNew()` / `linearBloomFree()` / `linearBloomReset()` (clears every bit).
- `linearBloomHashSet(bloom, hash)` adds the key and returns whether every one of its bits was already
  set (the key was possibly there before).
- `linearBloomHashCheck(bloom, hash)` tests every bit with no early exit (no branch per probe);
  `linearBloomHashCheckEarlyExit` stops at the first clear bit, faster when most queries miss.
- `linearBloomUtilization(bloom)` is the fraction of bits set.

A Bloom filter has no delete: clearing a key's bits would clear other keys'. Replace a filter that has
filled (utilization past about 0.5) with a fresh one.

## linearBloomCount

A counting filter: each slot is a 3-bit counter, and a key's count is the smallest of its 13 counters.

- `linearBloomCountHashSet(bloom, hash)` raises only the counters at the key's current minimum
  (conservative update, which keeps the estimate tight); counters saturate at 7.
- `linearBloomCountHashCheck(bloom, hash)` returns the key's count, 0 to 7: 0 means never added, any
  other value can overcount (another key sharing every counter), never undercount.
- Decay ages the counts so the filter tracks recent activity:
  - `linearBloomCountHalf`, `linearBloomCountQuarter`, `linearBloomCountEighth` divide every counter by
    2, 4 or 8 (word-at-a-time, with NEON, SSE2 and AVX2 versions of the halving);
  - `linearBloomCountDecayByFactor(bloom, factor, seed)` multiplies by a factor in [0, 1] with
    probabilistic rounding (2.7 becomes 3 seven times in ten), so the expected count is kept;
    `linearBloomCountDecayByFactorDeterministic` rounds down;
  - `linearBloomCountDecay(bloom, elapsedMs, halfLifeMs, seed)` decays by
    `2^(-elapsed / halfLife)`, and `linearBloomCountDecayDeterministic` with floor rounding.
- `linearBloomCountReset` clears every counter.

```c
#include "linearBloomCount.h"

/* Requests per key over the last hour or so: add on each request, halve
 * the counts every hour */
static void countExample(const uint64_t hash[2], uint64_t msSinceDecay) {
    static linearBloomCount *recent;
    if (!recent) {
        recent = linearBloomCountNew();
    }
    linearBloomCountHashSet(recent, (uint64_t *)hash);
    linearBloomCountDecay(recent, msSinceDecay, 60 * 60 * 1000, 0);
    const uint32_t hits = (uint32_t)linearBloomCountHashCheck(recent, hash);
    (void)hits; /* 0-7, an upper bound */
}
```

## Sizing

The size and hash count are compile-time constants. Define them before including the header to change
them for that file:

| Filter             | Size macro                        | Hashes macro              |
| ------------------ | --------------------------------- | ------------------------- |
| `linearBloom`      | `LINEARBLOOM_EXTENT_BITS`         | `LINEARBLOOM_HASHES`      |
| `linearBloomCount` | `LINEARBLOOMCOUNT_EXTENT_ENTRIES` | `LINEARBLOOMCOUNT_HASHES` |

With `n` keys in `m` slots and `k` hashes the false-positive rate is about `(1 - e^(-k*n/m))^k`; the
defaults give about 1 in 10,000 at the capacities in the table above.

## See also

- [hyperloglog](HYPERLOGLOG.md) - how many distinct keys, in 12 KB
- [multiroar](../multi/MULTIROAR.md) - exact sets of integers
- [rapidhash](../utils/RAPIDHASH.md) - the hash pair
