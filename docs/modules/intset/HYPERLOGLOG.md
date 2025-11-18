# HyperLogLog - Probabilistic Cardinality Estimation

## Overview

`hyperloglog` is a **probabilistic data structure for estimating the cardinality (count of unique elements) in a set**. It provides approximate counts with remarkable memory efficiency - typically using only 12 KB to count billions of unique items with ~0.81% standard error.

**Key Features:**
- Estimates unique counts with ~0.81% standard error
- Fixed memory usage (12 KB in dense mode)
- Two representations: sparse (memory-efficient for low cardinality) and dense (fixed size)
- Automatic promotion from sparse to dense
- Can merge multiple HyperLogLogs
- Handles billions of elements
- Redis-compatible implementation

**Header**: `hyperloglog.h`

**Source**: `hyperloglog.c`

**Algorithm**: Based on "HyperLogLog: the analysis of a near-optimal cardinality estimation algorithm" by Flajolet et al., with improvements from Heule, Nunkesser, and Hall

**Origin**: Adapted from Redis (Salvatore Sanfilippo)

## What is HyperLogLog?

### The Cardinality Problem

Imagine you need to count unique visitors to a website with billions of page views. Traditional approaches:

1. **Hash Set** - Store all unique IDs
   - Perfect accuracy
   - Memory: ~8 bytes per unique visitor
   - For 1 billion visitors: ~8 GB RAM

2. **HyperLogLog** - Probabilistic estimation
   - ~0.81% error
   - Memory: 12 KB regardless of count
   - For 1 billion visitors: still 12 KB!

**HyperLogLog trades perfect accuracy for massive memory savings.**

### How It Works (Simplified)

The algorithm is based on a clever observation:

```
If you flip a coin repeatedly, the longest run of heads you see
correlates with how many times you flipped the coin.

Saw 5 heads in a row? You probably flipped ~32 times (2^5).
Saw 10 heads in a row? You probably flipped ~1024 times (2^10).
```

HyperLogLog does this with hashed values:
1. Hash each element
2. Count leading zeros in binary representation
3. Track the maximum leading zeros seen
4. Estimate cardinality as 2^(max_zeros)

To reduce variance, it uses **16,384 separate "buckets"** (called registers) and averages their estimates.

## Data Structure

### Internal Representation

```c
typedef struct hyperloglogHeader {
    uint64_t cardinality : 61;     /* Cached cardinality estimate */
    uint64_t cardinalityValid : 1; /* Is cached value valid? */
    uint64_t encoding : 2;         /* SPARSE, DENSE, or RAW */
    uint8_t registers[];           /* Variable-length register data */
} hyperloglogHeader;
```

**Encoding Types:**
- **SPARSE** - Run-length encoded, memory-efficient for low cardinality
- **DENSE** - Fixed 12 KB, uses 16,384 6-bit registers
- **RAW** - Internal-only encoding for merging

### Memory Layout

**Sparse Representation** (for low cardinality):
```
Low cardinality (< ~3000 unique elements):
+----------+----------+-------------------+
| header   | encoding | compressed data   |
| (8 bytes)| (sparse) | (variable size)   |
+----------+----------+-------------------+
Size: 8 bytes to ~4 KB (auto-promotes to dense at 4 KB)

Example with 100 unique elements: ~267 bytes
Example with 1000 unique elements: ~1882 bytes
```

**Dense Representation** (for high cardinality):
```
High cardinality (>= ~3000 unique elements):
+----------+----------+------------------+
| header   | encoding | 16384 registers  |
| (8 bytes)| (dense)  | (12,288 bytes)   |
+----------+----------+------------------+
Fixed size: 12,296 bytes regardless of cardinality

Each register: 6 bits (values 0-63)
Total registers: 16,384 (2^14)
Register storage: (16384 * 6) / 8 = 12,288 bytes
```

### Register Encoding

**Dense Format** - Packed 6-bit values:
```
Bits:  543210 543210 543210 543210
      +------+------+------+------+
Byte: |  R0  |  R1  |  R2  |  R3  | ...
      +------+------+------+------+

Example (showing bit layout):
+--------+--------+--------+
|11000000|22221111|33333322|  <- Bytes
+--------+--------+--------+
 └─R0──┘                      Register 0: bits 0-5 of byte 0
        └────R1─────┘         Register 1: bits 6-7 of byte 0, bits 0-3 of byte 1
              └────R2─────┘   Register 2: bits 4-7 of byte 1, bits 0-1 of byte 2
                   └─R3──┘    Register 3: bits 2-7 of byte 2
```

**Sparse Format** - Run-length encoding with three opcodes:

```
ZERO:  00xxxxxx          - 1-64 zeros
XZERO: 01xxxxxx yyyyyyyy - 1-16384 zeros
VAL:   1vvvvvxx          - Value (1-32) repeated (1-4) times

Example sparse encoding for mostly empty HLL:
XZERO:1000    (registers 0-999 = 0)
VAL:2,1       (register 1000 = 2)
ZERO:19       (registers 1001-1019 = 0)
VAL:3,2       (registers 1020-1021 = 3)
XZERO:15362   (registers 1022-16383 = 0)
```

## API Reference

### Creation and Destruction

```c
/* Create new HyperLogLog (starts in sparse mode) */
hyperloglog *hyperloglogNew(void);
hyperloglog *hyperloglogNewSparse(void);  /* Alias for hyperloglogNew() */

/* Create HyperLogLog in dense mode */
hyperloglog *hyperloglogNewDense(void);

/* Deep copy */
hyperloglog *hyperloglogCopy(const hyperloglog *src);

/* Free HyperLogLog */
void hyperloglogFree(hyperloglog *h);

/* Example */
hyperloglog *hll = hyperloglogNew();
/* ... use it ... */
hyperloglogFree(hll);
```

### Adding Elements

```c
/* Add element to HyperLogLog
 * Returns: 1 if register changed, 0 if no change, -1 on error
 */
int hyperloglogAdd(hyperloglog **hh, const void *data, size_t size);

/* Example: Adding elements */
hyperloglog *hll = hyperloglogNew();

const char *user1 = "alice@example.com";
hyperloglogAdd(&hll, user1, strlen(user1));

const char *user2 = "bob@example.com";
hyperloglogAdd(&hll, user2, strlen(user2));

/* Adding same element again */
int changed = hyperloglogAdd(&hll, user1, strlen(user1));
// changed = 0 (no register change, already seen similar hash)

hyperloglogFree(hll);
```

### Counting Cardinality

```c
/* Get cardinality estimate
 * invalid: optional pointer, set to true if HLL is corrupted
 * Returns: estimated count of unique elements
 */
uint64_t hyperloglogCount(hyperloglog *h, bool *invalid);

/* Example */
hyperloglog *hll = hyperloglogNew();

for (int i = 0; i < 10000; i++) {
    hyperloglogAdd(&hll, &i, sizeof(i));
}

bool invalid = false;
uint64_t estimate = hyperloglogCount(hll, &invalid);
printf("Estimated unique count: %lu\n", estimate);
printf("Actual count: 10000\n");
printf("Error: %.2f%%\n", fabs(estimate - 10000.0) / 10000.0 * 100);
// Typically within ~0.8% of actual

hyperloglogFree(hll);
```

### Merging HyperLogLogs

```c
/* Merge 'src' into 'target'
 * Returns: true on success, false if HLL is invalid
 */
bool hyperloglogMerge(hyperloglog *target, const hyperloglog *src);

/* Example: Merge counts from multiple sources */
hyperloglog *serverA = hyperloglogNew();
hyperloglog *serverB = hyperloglogNew();

/* Server A sees users 1-100 */
for (int i = 1; i <= 100; i++) {
    hyperloglogAdd(&serverA, &i, sizeof(i));
}

/* Server B sees users 51-150 (50 overlap) */
for (int i = 51; i <= 150; i++) {
    hyperloglogAdd(&serverB, &i, sizeof(i));
}

/* Merge to get total unique count */
hyperloglog *total = hyperloglogCopy(serverA);
hyperloglogMerge(total, serverB);

uint64_t count = hyperloglogCount(total, NULL);
printf("Estimated unique users: %lu\n", count);
// Should be ~150 (actual unique: 1-150)

hyperloglogFree(serverA);
hyperloglogFree(serverB);
hyperloglogFree(total);
```

### Utility Functions

```c
/* Invalidate cached cardinality (forces recomputation) */
void hyperloglogInvalidateCache(hyperloglog *h);

/* Validate HyperLogLog structure
 * Returns: true if valid, false if corrupted
 */
bool hyperloglogDetect(hyperloglog *h);

/* Example */
hyperloglog *hll = hyperloglogNew();
hyperloglogAdd(&hll, "test", 4);

if (hyperloglogDetect(hll)) {
    printf("HLL is valid\n");
} else {
    printf("HLL is corrupted!\n");
}

hyperloglogFree(hll);
```

## User-Friendly API (pfadd, pfcount, pfmerge)

Redis-compatible convenience functions with variadic arguments:

```c
/* Add multiple elements (NULL-terminated)
 * Format: pfadd(&hll, data1, len1, data2, len2, ..., NULL)
 * Returns: number of registers modified
 */
int pfadd(hyperloglog **hh, ...);

/* Count single HLL */
uint64_t pfcountSingle(hyperloglog *h);

/* Count merged cardinality of multiple HLLs (NULL-terminated) */
uint64_t pfcount(hyperloglog *h, ...);

/* Merge multiple HLLs (NULL-terminated)
 * Returns: newly allocated merged HyperLogLog
 */
hyperloglog *pfmerge(hyperloglog *h, ...);
```

### Example: Using Friendly API

```c
/* Add multiple elements at once */
hyperloglog *visitors = hyperloglogNew();

const char *user1 = "alice", *user2 = "bob", *user3 = "charlie";
int updated = pfadd(&visitors,
    user1, strlen(user1),
    user2, strlen(user2),
    user3, strlen(user3),
    NULL);  // NULL terminates the list

printf("Updated %d registers\n", updated);

/* Count */
uint64_t count = pfcountSingle(visitors);
printf("Unique visitors: %lu\n", count);

/* Merge multiple HLLs */
hyperloglog *monday = hyperloglogNew();
hyperloglog *tuesday = hyperloglogNew();

pfadd(&monday, "user1", 5, "user2", 5, NULL);
pfadd(&tuesday, "user2", 5, "user3", 5, NULL);

hyperloglog *week = pfmerge(monday, tuesday, NULL);
count = pfcountSingle(week);
printf("Unique visitors this week: %lu\n", count);  // ~3

hyperloglogFree(visitors);
hyperloglogFree(monday);
hyperloglogFree(tuesday);
hyperloglogFree(week);
```

## Sparse vs Dense Modes

### Sparse Mode (Default)

**Characteristics:**
- Memory efficient for low cardinality
- Variable size (grows as elements added)
- Uses run-length encoding
- Automatically promotes to dense when:
  - Register value exceeds 32 (requires 33-63)
  - Sparse size exceeds ~4 KB

**Memory Efficiency:**

| Unique Elements | Avg Sparse Size |
|-----------------|-----------------|
| 100 | 267 bytes |
| 500 | 1,033 bytes |
| 1,000 | 1,882 bytes |
| 2,000 | 3,480 bytes |
| 3,000 | 4,879 bytes |
| 5,000+ | → Dense (12 KB) |

### Dense Mode

**Characteristics:**
- Fixed 12,296 bytes
- Faster access (no decompression)
- Better for high cardinality
- Cannot convert back to sparse

**When Promotion Happens:**

```c
hyperloglog *hll = hyperloglogNew();
printf("Initial: sparse mode\n");

/* Add elements with small hash values - stays sparse */
for (int i = 0; i < 1000; i++) {
    hyperloglogAdd(&hll, &i, sizeof(i));
}
// Still sparse (~1882 bytes)

/* Keep adding until size threshold or large register value */
for (int i = 1000; i < 5000; i++) {
    hyperloglogAdd(&hll, &i, sizeof(i));
}
// Promoted to dense (12 KB)

hyperloglogFree(hll);
```

### Comparison

```c
/* Sparse example - efficient for low cardinality */
hyperloglog *sparse = hyperloglogNew();
for (int i = 0; i < 1000; i++) {
    hyperloglogAdd(&sparse, &i, sizeof(i));
}
// Size: ~1882 bytes
// Estimate: ~1000

/* Dense example - efficient for high cardinality */
hyperloglog *dense = hyperloglogNewDense();
for (int i = 0; i < 1000000; i++) {
    hyperloglogAdd(&dense, &i, sizeof(i));
}
// Size: 12,296 bytes
// Estimate: ~1,000,000

/* Memory savings */
printf("Sparse: ~2 KB for 1K elements\n");
printf("Dense: 12 KB for 1M elements\n");
printf("Savings vs hash set: %.1fx\n", (1000000.0 * 8) / 12296);
// Savings: ~650x for 1M elements!

hyperloglogFree(sparse);
hyperloglogFree(dense);
```

## Real-World Examples

### Example 1: Website Unique Visitors

```c
/* Count unique visitors across multiple pages */
typedef struct analytics {
    hyperloglog *totalVisitors;
    hyperloglog *pageVisitors[100];  /* Per-page counts */
} analytics;

analytics *analyticsNew(void) {
    analytics *a = malloc(sizeof(*a));
    a->totalVisitors = hyperloglogNew();
    for (int i = 0; i < 100; i++) {
        a->pageVisitors[i] = hyperloglogNew();
    }
    return a;
}

void analyticsRecordVisit(analytics *a, int pageId, const char *visitorId) {
    size_t len = strlen(visitorId);

    /* Record in total */
    hyperloglogAdd(&a->totalVisitors, visitorId, len);

    /* Record in page-specific counter */
    if (pageId >= 0 && pageId < 100) {
        hyperloglogAdd(&a->pageVisitors[pageId], visitorId, len);
    }
}

void analyticsPrintStats(analytics *a) {
    uint64_t total = hyperloglogCount(a->totalVisitors, NULL);
    printf("Total unique visitors: %lu\n", total);

    for (int i = 0; i < 100; i++) {
        uint64_t pageCount = hyperloglogCount(a->pageVisitors[i], NULL);
        if (pageCount > 0) {
            printf("  Page %d: %lu unique visitors\n", i, pageCount);
        }
    }
}

void analyticsFree(analytics *a) {
    hyperloglogFree(a->totalVisitors);
    for (int i = 0; i < 100; i++) {
        hyperloglogFree(a->pageVisitors[i]);
    }
    free(a);
}

/* Usage */
analytics *stats = analyticsNew();

/* Simulate traffic */
analyticsRecordVisit(stats, 0, "user123");
analyticsRecordVisit(stats, 0, "user456");
analyticsRecordVisit(stats, 1, "user123");  /* Same user, different page */
analyticsRecordVisit(stats, 0, "user789");

analyticsPrintStats(stats);
/* Output:
 * Total unique visitors: 3
 *   Page 0: 3 unique visitors
 *   Page 1: 1 unique visitors
 */

analyticsFree(stats);
```

### Example 2: IP Address Deduplication

```c
/* Count unique IP addresses in log files */
typedef struct ipCounter {
    hyperloglog *uniqueIPs;
    uint64_t totalRequests;
} ipCounter;

ipCounter *ipCounterNew(void) {
    ipCounter *ic = malloc(sizeof(*ic));
    ic->uniqueIPs = hyperloglogNew();
    ic->totalRequests = 0;
    return ic;
}

void ipCounterAdd(ipCounter *ic, const char *ipAddress) {
    hyperloglogAdd(&ic->uniqueIPs, ipAddress, strlen(ipAddress));
    ic->totalRequests++;
}

void ipCounterPrintStats(ipCounter *ic) {
    uint64_t unique = hyperloglogCount(ic->uniqueIPs, NULL);
    printf("Total requests: %lu\n", ic->totalRequests);
    printf("Unique IPs: %lu\n", unique);
    printf("Avg requests per IP: %.2f\n",
           (double)ic->totalRequests / unique);
}

void ipCounterFree(ipCounter *ic) {
    hyperloglogFree(ic->uniqueIPs);
    free(ic);
}

/* Usage: Process log file */
ipCounter *counter = ipCounterNew();

/* Simulate log entries */
const char *ips[] = {
    "192.168.1.1", "192.168.1.2", "192.168.1.1",
    "10.0.0.5", "192.168.1.1", "10.0.0.6"
};

for (int i = 0; i < 6; i++) {
    ipCounterAdd(counter, ips[i]);
}

ipCounterPrintStats(counter);
/* Output:
 * Total requests: 6
 * Unique IPs: 4
 * Avg requests per IP: 1.50
 */

ipCounterFree(counter);
```

### Example 3: Multi-Server Aggregation

```c
/* Aggregate unique counts from multiple servers */
typedef struct serverMetrics {
    hyperloglog *hll;
    char serverName[64];
} serverMetrics;

serverMetrics *metricsNew(const char *name) {
    serverMetrics *m = malloc(sizeof(*m));
    m->hll = hyperloglogNew();
    strncpy(m->serverName, name, sizeof(m->serverName) - 1);
    return m;
}

void metricsAddEvent(serverMetrics *m, uint64_t eventId) {
    hyperloglogAdd(&m->hll, &eventId, sizeof(eventId));
}

hyperloglog *metricsAggregateAll(serverMetrics **servers, int count) {
    if (count == 0) return NULL;

    hyperloglog *aggregate = hyperloglogCopy(servers[0]->hll);

    for (int i = 1; i < count; i++) {
        hyperloglogMerge(aggregate, servers[i]->hll);
    }

    return aggregate;
}

void metricsFree(serverMetrics *m) {
    hyperloglogFree(m->hll);
    free(m);
}

/* Usage: Multi-datacenter analytics */
serverMetrics *dc1 = metricsNew("DC-US-East");
serverMetrics *dc2 = metricsNew("DC-US-West");
serverMetrics *dc3 = metricsNew("DC-EU");

/* Each datacenter sees different events with some overlap */
for (uint64_t i = 1; i <= 100; i++) metricsAddEvent(dc1, i);
for (uint64_t i = 50; i <= 150; i++) metricsAddEvent(dc2, i);
for (uint64_t i = 100; i <= 200; i++) metricsAddEvent(dc3, i);

serverMetrics *servers[] = {dc1, dc2, dc3};
hyperloglog *global = metricsAggregateAll(servers, 3);

printf("DC-US-East: %lu unique events\n",
       hyperloglogCount(dc1->hll, NULL));  // ~100
printf("DC-US-West: %lu unique events\n",
       hyperloglogCount(dc2->hll, NULL));  // ~100
printf("DC-EU: %lu unique events\n",
       hyperloglogCount(dc3->hll, NULL));  // ~100
printf("Global (merged): %lu unique events\n",
       hyperloglogCount(global, NULL));  // ~200

metricsFree(dc1);
metricsFree(dc2);
metricsFree(dc3);
hyperloglogFree(global);
```

### Example 4: Streaming Data Cardinality

```c
/* Track unique elements in a data stream */
typedef struct streamCounter {
    hyperloglog *current;
    hyperloglog *last5min;
    hyperloglog *lastHour;
    time_t lastReset;
} streamCounter;

streamCounter *streamCounterNew(void) {
    streamCounter *sc = malloc(sizeof(*sc));
    sc->current = hyperloglogNew();
    sc->last5min = hyperloglogNew();
    sc->lastHour = hyperloglogNew();
    sc->lastReset = time(NULL);
    return sc;
}

void streamCounterAdd(streamCounter *sc, const void *data, size_t len) {
    hyperloglogAdd(&sc->current, data, len);
    hyperloglogAdd(&sc->last5min, data, len);
    hyperloglogAdd(&sc->lastHour, data, len);
}

void streamCounterRotate(streamCounter *sc) {
    time_t now = time(NULL);

    /* Every 5 minutes */
    if (now - sc->lastReset >= 300) {
        hyperloglogFree(sc->last5min);
        sc->last5min = sc->current;
        sc->current = hyperloglogNew();
        sc->lastReset = now;
    }

    /* Every hour */
    if (now - sc->lastReset >= 3600) {
        hyperloglogFree(sc->lastHour);
        sc->lastHour = sc->last5min;
        sc->last5min = hyperloglogNew();
    }
}

void streamCounterPrintStats(streamCounter *sc) {
    printf("Current window: %lu unique\n",
           hyperloglogCount(sc->current, NULL));
    printf("Last 5 minutes: %lu unique\n",
           hyperloglogCount(sc->last5min, NULL));
    printf("Last hour: %lu unique\n",
           hyperloglogCount(sc->lastHour, NULL));
}

void streamCounterFree(streamCounter *sc) {
    hyperloglogFree(sc->current);
    hyperloglogFree(sc->last5min);
    hyperloglogFree(sc->lastHour);
    free(sc);
}
```

## Algorithm Explanation

### The Mathematics

HyperLogLog uses the distribution of leading zeros in hashed values:

```
For a uniformly random bit string:
- P(starts with 0)      = 1/2    → see 0 in ~2 elements
- P(starts with 00)     = 1/4    → see 00 in ~4 elements
- P(starts with 000)    = 1/8    → see 000 in ~8 elements
- P(starts with k zeros) = 1/2^k → see k zeros in ~2^k elements

So: max_zeros ≈ log2(number_of_unique_elements)
```

**Estimation formula:**
```
cardinality ≈ 2^(average_max_zeros) * correction_factor
```

### Step-by-Step Process

```c
/* Simplified HyperLogLog logic */

// 1. Hash the input
uint64_t hash = XXH3_64bits(data, len);

// 2. Use first 14 bits for register index (2^14 = 16384 registers)
uint32_t register_index = hash & 0x3FFF;  // Last 14 bits

// 3. Count leading zeros in remaining 50 bits + 1
hash >>= 14;  // Remove register bits
int leading_zeros = __builtin_clzll(hash | (1ULL << 50)) + 1;

// 4. Update register if this is a new maximum
if (leading_zeros > registers[register_index]) {
    registers[register_index] = leading_zeros;
}

// 5. To estimate cardinality, use harmonic mean of 2^register values
// (actual algorithm uses improved estimator by Ertl)
```

### Why 16,384 Registers?

The number of registers (m = 16,384 = 2^14) provides:
```
Standard error = 1.04 / sqrt(m)
               = 1.04 / sqrt(16384)
               = 1.04 / 128
               ≈ 0.81%
```

More registers → lower error but more memory.

### Error Characteristics

```c
/* Demonstrate error distribution */
hyperloglog *hll = hyperloglogNew();
int actual = 10000;

for (int i = 0; i < actual; i++) {
    hyperloglogAdd(&hll, &i, sizeof(i));
}

uint64_t estimate = hyperloglogCount(hll, NULL);
double error = (double)(estimate - actual) / actual * 100;

printf("Actual: %d\n", actual);
printf("Estimated: %lu\n", estimate);
printf("Error: %.2f%%\n", error);
// Typically: Error < ±1%

hyperloglogFree(hll);
```

**Error bound:**
- Standard error: ~0.81%
- Typical range: ±1-2% for most counts
- 99% confidence: within ±3 standard errors (~2.4%)

## Performance Characteristics

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Create | O(1) sparse, O(1) dense | Small sparse init, fixed dense |
| Add (sparse) | O(log n) | Binary search + insertion |
| Add (dense) | O(1) | Direct register update |
| Count (sparse) | O(n) | Decompress and compute |
| Count (dense) | O(m) | m = 16,384 registers |
| Merge | O(m) | Compare all 16,384 registers |
| Memory | O(1) | 12 KB dense, variable sparse |

**Sparse → Dense Promotion:**
- Adds O(n) overhead when triggered
- Only happens once per HLL
- Configurable threshold (~4 KB default)

## Memory Efficiency

### Comparison with Exact Counting

```
For 1,000,000 unique elements:

Hash Set:
- Memory: 1,000,000 * 8 = 8 MB (minimum)
- With overhead: ~16 MB (load factor, pointers)
- Accuracy: Perfect (100%)

HyperLogLog:
- Memory: 12,296 bytes = 12 KB
- Accuracy: ~99.2% (±0.8% error)
- Memory savings: ~1,300x

For 1,000,000,000 unique elements:

Hash Set:
- Memory: ~16 GB
- Accuracy: Perfect

HyperLogLog:
- Memory: 12 KB (same!)
- Accuracy: ~99.2% (same!)
- Memory savings: ~1,300,000x
```

### When to Use HyperLogLog

**Use HyperLogLog when:**
- Counting billions of unique items
- Memory is limited
- ~1% error is acceptable
- Need to merge counts from multiple sources
- Approximate count is sufficient

**Don't use HyperLogLog when:**
- Need exact counts
- Small cardinality (< 1000) where memory doesn't matter
- Need to retrieve individual elements
- Need set membership testing

## Best Practices

### 1. Understand the Error Margin

```c
/* HyperLogLog is probabilistic - results vary */
hyperloglog *hll = hyperloglogNew();

for (int i = 0; i < 100; i++) {
    hyperloglogAdd(&hll, &i, sizeof(i));
}

uint64_t count = hyperloglogCount(hll, NULL);
// count might be: 98, 99, 100, 101, or 102
// All are valid within error bounds!

/* For small counts, error percentage can be larger */
printf("Count: %lu (expected ~100, ±1%%)\n", count);

hyperloglogFree(hll);
```

### 2. Use Sparse Mode for Low Cardinality

```c
/* GOOD - sparse mode for small sets */
hyperloglog *sparse = hyperloglogNew();  // Starts sparse
for (int i = 0; i < 1000; i++) {
    hyperloglogAdd(&sparse, &i, sizeof(i));
}
// Uses ~1882 bytes

/* LESS EFFICIENT - dense mode for small sets */
hyperloglog *dense = hyperloglogNewDense();
for (int i = 0; i < 1000; i++) {
    hyperloglogAdd(&dense, &i, sizeof(i));
}
// Uses 12,296 bytes (6.5x more memory)
```

### 3. Hash Inputs Properly

```c
/* GOOD - use full data for hashing */
typedef struct user {
    uint64_t id;
    char email[100];
} user;

hyperloglog *hll = hyperloglogNew();
user u = {123, "user@example.com"};
hyperloglogAdd(&hll, &u, sizeof(u));  // Hash entire struct

/* BAD - only hash ID (might have collisions) */
hyperloglogAdd(&hll, &u.id, sizeof(u.id));
// If IDs are sequential, hash distribution might be poor
```

### 4. Merge for Distributed Counting

```c
/* Collect counts from multiple sources */
hyperloglog *sources[10];
for (int i = 0; i < 10; i++) {
    sources[i] = hyperloglogNew();
    /* Each source adds its own data */
}

/* Merge all sources */
hyperloglog *total = hyperloglogCopy(sources[0]);
for (int i = 1; i < 10; i++) {
    hyperloglogMerge(total, sources[i]);
}

uint64_t combined = hyperloglogCount(total, NULL);
printf("Total unique across all sources: %lu\n", combined);

/* Clean up */
for (int i = 0; i < 10; i++) {
    hyperloglogFree(sources[i]);
}
hyperloglogFree(total);
```

## Common Pitfalls

### 1. Expecting Exact Counts

```c
/* WRONG EXPECTATION */
hyperloglog *hll = hyperloglogNew();
hyperloglogAdd(&hll, "test", 4);
uint64_t count = hyperloglogCount(hll, NULL);
assert(count == 1);  // MAY FAIL! Count might be 0, 1, or 2

/* RIGHT EXPECTATION */
if (count >= 1 && count <= 2) {
    // Within expected error range
}
```

### 2. Using for Small Exact Counts

```c
/* BAD - HLL not needed for small exact counts */
hyperloglog *hll = hyperloglogNew();
for (int i = 0; i < 10; i++) {
    hyperloglogAdd(&hll, &i, sizeof(i));
}
// Estimate: might be 9, 10, or 11

/* GOOD - use hash set for small exact counts */
intset *exact = intsetNew();
for (int i = 0; i < 10; i++) {
    intsetAdd(&exact, i, NULL);
}
// Count: exactly 10
```

### 3. Forgetting Merge Semantics

```c
/* Merge computes UNION, not intersection */
hyperloglog *a = hyperloglogNew();
hyperloglog *b = hyperloglogNew();

hyperloglogAdd(&a, "x", 1);
hyperloglogAdd(&b, "y", 1);

hyperloglog *merged = hyperloglogCopy(a);
hyperloglogMerge(merged, b);

// merged estimates ~2 (union of {x} and {y})
// NOT 0 (intersection would be empty)
```

## See Also

- [intset](INTSET.md) - Exact counting with sorted integers
- [intsetU32](INTSET_U32.md) - Fixed 32-bit integer sets
- [intsetBig](INTSET_BIG.md) - Large integer sets

## Testing

Run the HyperLogLog test suite:

```bash
./src/datakit-test test hyperloglog
```

The test suite validates:
- Register access and encoding
- Approximation error bounds
- Sparse to dense promotion
- Dense and sparse agreement
- Merge operations
- Edge cases and stress tests

## References

1. **Original Paper**: Flajolet, P., Fusy, É., Gandouet, O., & Meunier, F. (2007). "HyperLogLog: the analysis of a near-optimal cardinality estimation algorithm." *AofA: Analysis of Algorithms*, 127-146.

2. **Improved Estimator**: Heule, S., Nunkesser, M., & Hall, A. (2013). "HyperLogLog in Practice: Algorithmic Engineering of a State of The Art Cardinality Estimation Algorithm." *EDBT 2013*.

3. **Better Bias Correction**: Ertl, O. (2017). "New cardinality estimation algorithms for HyperLogLog sketches." *arXiv:1702.01284*.
