# Memory Efficiency Patterns

## Overview

datakit is designed for **exceptional memory efficiency** through compression, compact encodings, and careful structure design. This guide explains the memory patterns used throughout the library and how to minimize your application's memory footprint.

## Core Memory Principles

### 1. Pointer-Free Design

Traditional data structures use pointers extensively, wasting memory:

```
Traditional linked list node (64-bit system):
┌─────────┬─────────┬─────────┐
│ prev*   │ next*   │ data    │
│ 8 bytes │ 8 bytes │ N bytes │
└─────────┴─────────┴─────────┘
Overhead: 16 bytes per node

datakit multilist node:
┌─────────────────┐
│ flex with data  │
│ N bytes         │
└─────────────────┘
Overhead: ~6 bytes per flex
```

**Memory savings**: 62% overhead reduction for small nodes.

### 2. Variable-Length Encoding

datakit automatically selects optimal encoding:

```c
/* flex stores "hi" */
[0x02][0x68][0x69][0x02]  // 4 bytes total
  ^     ^     ^     ^
  |     |     |     └─ reverse encoding
  |     |     └─────── data: "hi"
  |     └───────────── forward encoding
  └─────────────────── length byte

/* Traditional: char* pointer + malloc */
char *s = malloc(3);  // "hi\0"
// Pointer: 8 bytes + malloc overhead (16-32 bytes) = 24-40 bytes
// vs flex: 4 bytes (6-10x savings!)
```

### 3. Compression Awareness

Multiple layers of compression:

1. **Structural**: Variable encoding, packed formats
2. **Algorithmic**: XOR filter (xof), delta-of-delta (dod)
3. **Generic**: LZ4 compression (multilist Full variant)
4. **Specialized**: Float16/bfloat16, intset encoding

## Memory Overhead by Module

### flex Overhead

```c
Empty flex: 2 bytes
[bytes varint][count varint]

Small flex with 3 integers:
Header: 2 bytes
Entry 1: 3 bytes (encoding + data + encoding)
Entry 2: 3 bytes
Entry 3: 3 bytes
Total: 11 bytes for 3 integers (vs 12 bytes for int[] + wasted padding)

Efficiency: ~8% overhead
```

### multimap Overhead

**Small variant** (< 100 entries):

```
Overhead: 16 bytes (fixed)
Data: 1 flex (variable)
Per-entry: ~2-4 bytes encoding overhead

Example with 50 key-value pairs (20 bytes each):
16 + 50*20 + 50*4 = 1,216 bytes
Overhead: 1.3%
```

**Medium variant** (100-1000 entries):

```
Overhead: 28 bytes (fixed)
Data: 2 flex (variable)

Example with 500 entries:
28 + 500*20 + 500*4 = 10,028 bytes
Overhead: 0.3%
```

**Full variant** (1000+ entries):

```
Overhead: 40 bytes (base) + (M * 20 bytes) where M = map count
Data: M flex (variable)

Example with 10,000 entries across 50 maps:
40 + (50*20) + 10,000*20 + 10,000*4 = 241,040 bytes
Overhead: 0.5%
```

### multilist Overhead

**Small**: 8 bytes + 1 flex
**Medium**: 16 bytes + 2 flex
**Full**: 24 bytes + N mflex nodes

**With compression** (Full variant only):

```c
/* Text data compression ratios */
No compression:     100%
compress=1:         40-50% (aggressive)
compress=2:         50-60% (balanced)
compress=5:         70-80% (light)

/* Example: 1 million log entries, 100 bytes each */
Uncompressed: 100 MB
compress=1:   40 MB (60% savings)
compress=2:   50 MB (50% savings)
```

### multiarray Overhead

**Native**: 0 bytes (just the array)
**Small**: 16 bytes
**Medium**: 16 bytes + (N nodes × 16 bytes)
**Large**: 24 bytes + (N nodes × 24 bytes)

```c
/* 10,000 elements, 16 bytes each */
Native:    0 + 160,000 = 160,000 bytes
Small:    16 + 160,000 = 160,016 bytes (0.01% overhead)
Medium:   16 + (20*16) + 160,000 = 160,336 bytes (0.2% overhead)
Large:    24 + (10*24) + 160,000 = 160,264 bytes (0.16% overhead)

/* Large uses XOR links: saves 8 bytes per node vs traditional */
```

### intset Overhead

```c
/* Header: 8 bytes (encoding + length) */
/* Data: length * encoding_size */

1000 values in INT16:
8 + 1000*2 = 2,008 bytes (0.4% overhead)

1000 values in INT32:
8 + 1000*4 = 4,008 bytes (0.2% overhead)

1000 values in INT64:
8 + 1000*8 = 8,008 bytes (0.1% overhead)

/* Compare to std::set<int64_t> */
std::set: ~40 bytes per node (pointer + color + padding)
= 40,000 bytes (5x more memory!)
```

### String Overhead

**mds (full string):**

```c
struct mds {
    size_t len;      // 8 bytes
    size_t alloc;    // 8 bytes
    uint32_t refcnt; // 4 bytes
    uint32_t flags;  // 4 bytes
    char buf[];      // N bytes
};
// Total: 24 bytes + N
```

**mdsc (compact string):**

```c
struct mdsc {
    size_t len;    // 8 bytes
    char buf[];    // N bytes
};
// Total: 8 bytes + N
```

**Comparison:**

```c
"hello" (5 chars)
mds:  24 + 5 + 1 (null) = 30 bytes
mdsc:  8 + 5 + 1 = 14 bytes (53% savings)
```

## Compression Techniques

### Variable-Width Integer Encoding (intset)

```c
/* Automatic sizing based on value range */
Values in [-32768, 32767]:   INT16 (2 bytes)
Values in [-2B, 2B]:         INT32 (4 bytes)
Any larger:                  INT64 (8 bytes)

/* Example: User IDs from 1-10,000 */
intset *ids = intsetNew();  // Starts INT16
for (int i = 1; i <= 10000; i++) {
    ids = intsetAdd(ids, i, NULL);
}
// All values fit in INT16: 8 + 10,000*2 = 20,008 bytes
// vs int64_t[10000] = 80,000 bytes (4x savings!)
```

### XOR Filter Compression (xof)

For time-series double values:

```c
/* Temperature readings every minute for 24 hours */
xofWriter writer = {...};
for (int i = 0; i < 1440; i++) {
    double temp = 20.0 + 5.0 * sin(i * M_PI / 720);
    xofWrite(&writer, temp);
}

/* Uncompressed: 1440 * 8 = 11,520 bytes */
/* XOF compressed: ~700-800 bytes */
/* Compression: 15-16x (93% savings) */
```

**Why it works**: Consecutive temperatures have similar bit patterns. XOR produces many zeros, which compress to 1-2 bits.

### Delta-of-Delta Encoding (dod)

For sequences of increasing integers:

```c
/* Timestamps: 1000000, 1000060, 1000120, 1000180... */
dod *d = dodNew();
for (int64_t ts = 1000000; ts < 2000000; ts += 60) {
    dodAdd(d, ts);
}

/* Original: ~16,667 values * 8 bytes = 133 KB */
/* Delta-of-delta: ~2-3 bytes per value = 33-50 KB */
/* Compression: 3-4x (70-75% savings) */
```

### Half-Precision Floats (float16/bfloat16)

```c
/* Graphics vertex buffer */
typedef struct Vertex {
    uint16_t pos[3];     // float16: x, y, z
    uint16_t normal[3];  // float16: nx, ny, nz
    uint16_t uv[2];      // float16: u, v
} Vertex;  // 16 bytes

/* vs traditional */
typedef struct VertexFull {
    float pos[3];     // 12 bytes
    float normal[3];  // 12 bytes
    float uv[2];      // 8 bytes
} VertexFull;  // 32 bytes

/* 1 million vertices */
float16: 16 MB
float32: 32 MB
Savings: 50% (16 MB)
```

**Trade-off**: ~3 decimal digits precision vs 7 digits.

### LZ4 Compression (multilist Full)

```c
/* Log entries with repeated text */
multilist *logs = multilistNew(FLEX_CAP_LEVEL_4096, 1);
// compress=1: aggressive compression

for (int i = 0; i < 100000; i++) {
    char entry[200];
    snprintf(entry, 200, "[INFO] User %d logged in at timestamp %ld",
             i % 1000, time(NULL));
    databox box = databoxNewBytesString(entry);
    multilistPushByTypeTail(&logs, state, &box);
}

/* Uncompressed: 100,000 * 200 = 20 MB */
/* LZ4 compressed: ~4-6 MB (70-80% savings) */
```

## Memory Allocation Strategies

### membound - Bounded Allocator

**Purpose**: Pre-allocate fixed pool, prevent unbounded growth.

```c
/* Create 100 MB pool */
membound *pool = memboundCreate(100 * 1024 * 1024);

/* All allocations come from pool */
void *data1 = memboundAlloc(pool, 1024);
void *data2 = memboundAlloc(pool, 2048);

/* Can't exceed 100 MB - allocations fail gracefully */
void *big = memboundAlloc(pool, 200 * 1024 * 1024);
// Returns NULL - insufficient space

/* Overhead */
Per-block: 1 byte control
Fragmentation: Bounded by Robson's theorem
```

**Use cases:**

- Embedded systems
- Container memory limits
- Preventing OOM kills
- Testing memory pressure

### fibbuf - Fibonacci Growth

```c
/* Growth sequence: 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144... */
fibbuf *fb = fibbufNew();

/* First allocation: 1 unit */
void *mem1 = fibbufGrow(fb);  // Size: 1

/* Second: 2 units (2x) */
void *mem2 = fibbufGrow(fb);  // Size: 2

/* Third: 3 units (1.5x) */
void *mem3 = fibbufGrow(fb);  // Size: 3

/* Eventually approaches golden ratio (~1.618x) */
```

**Advantages over 2x doubling:**

- Less aggressive growth (less memory waste)
- Still amortized O(1)
- Better for medium-sized datasets

### jebuf - jemalloc Size Class Alignment

```c
/* Align allocations to jemalloc size classes */
jebuf *jb = jebufNew();

/* Request 1000 bytes */
void *mem = jebufAlloc(jb, 1000);
// Actually allocates 1024 (next jemalloc size class)
// Avoids internal fragmentation in jemalloc
```

**jemalloc size classes**: 8, 16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024...

## Memory Efficiency Patterns

### Pattern 1: String Deduplication

**Problem**: Many duplicate strings waste memory.

**Solution**: Use multimapAtom for string interning.

```c
multimapAtom *atoms = multimapAtomNew();

/* Add "common-key" 1000 times */
for (int i = 0; i < 1000; i++) {
    databox key = databoxNewBytesString("common-key");
    databox val = databoxNewSigned(i);

    /* Convert key to atom reference (deduplicated) */
    multimapAtomInsertIfNewConvert(atoms, &key);
    // key is now a small integer reference!

    multimapInsert(&m, (const databox*[]){&key, &val});
}

/* Without deduplication: 1000 * 10 = 10,000 bytes for keys */
/* With deduplication: 10 bytes + 1000 * 2 = 2,010 bytes */
/* Savings: 80% */
```

### Pattern 2: Compression for Archival Data

```c
/* Recent data: uncompressed for speed */
multilist *recent = multilistNew(FLEX_CAP_LEVEL_4096, 10);
// compress=10: fast access to last 10 nodes each end

/* Archive data: compressed for space */
multilist *archive = multilistNew(FLEX_CAP_LEVEL_4096, 1);
// compress=1: aggressive compression

/* Move old data to archive */
while (multilistCount(recent) > 10000) {
    databox item;
    multilistPopByTypeHead(&recent, state, &item);
    multilistPushByTypeTail(&archive, state, &item);
}
```

### Pattern 3: Native Arrays for Fixed-Size Data

```c
/* AVOID: Dynamic array for known size */
multiarraySmall *arr = multiarraySmallNew(sizeof(Point), 1000);
// 16 bytes overhead

/* USE: Native array */
typedef struct Point { float x, y, z; } Point;
Point *points = multiarrayNativeNew(points[1000]);
// 0 bytes overhead
// Auto-upgrades to Medium if you exceed 1000
```

### Pattern 4: Batch Operations to Reduce Allocations

```c
/* BAD: Individual inserts */
flex *f = flexNew();
for (int i = 0; i < 1000; i++) {
    databox box = databoxNewSigned(i);
    flexPushByType(&f, &box, FLEX_ENDPOINT_TAIL);
    // Potential realloc each time
}

/* GOOD: Build batch, append once */
flex *batch = flexNew();
batch = zrealloc(batch, expected_size);  // Preallocate
for (int i = 0; i < 1000; i++) {
    databox box = databoxNewSigned(i);
    flexPushByType(&batch, &box, FLEX_ENDPOINT_TAIL);
    // No reallocs
}
flexBulkAppendFlex(&f, batch);
```

### Pattern 5: Choose Encoding Based on Value Range

```c
/* Small values: use intset */
intset *small_ids = intsetNew();
for (int i = 1; i <= 30000; i++) {
    small_ids = intsetAdd(small_ids, i, NULL);
}
// Uses INT16: 8 + 30,000*2 = 60,008 bytes

/* Large values: add largest first to force encoding */
intset *large_ids = intsetNew();
large_ids = intsetAdd(large_ids, 5000000000L, NULL);  // Force INT64
for (int64_t i = 1; i <= 30000; i++) {
    large_ids = intsetAdd(large_ids, i, NULL);
}
// Uses INT64: 8 + 30,001*8 = 240,016 bytes
// But all values are representable
```

## Memory Profiling

### Measuring Memory Usage

```c
/* Per-module memory queries */
size_t flexMem = flexBytes(f);
size_t multimapMem = multimapBytes(m);
size_t multilistMem = multilistBytes(ml);
size_t multiarrayMem = multiarrayBytes(ma);
size_t intsetMem = intsetBytes(is);

/* membound statistics */
size_t outstanding = memboundCurrentAllocationCount(pool);
```

### Memory Profiling Example

```c
typedef struct MemoryProfile {
    const char *component;
    size_t bytes;
} MemoryProfile;

void printMemoryBreakdown(void) {
    MemoryProfile profile[] = {
        {"User cache", multimapBytes(userCache)},
        {"Message queue", multilistBytes(messageQueue)},
        {"Event log", multiarrayBytes(eventLog)},
        {"Active sessions", intsetBytes(activeSessions)},
        {"Sensor data", xofWriterBytes(&sensorData)},
    };

    size_t total = 0;
    for (int i = 0; i < 5; i++) {
        total += profile[i].bytes;
        printf("%s: %.2f KB (%.1f%%)\n",
               profile[i].component,
               profile[i].bytes / 1024.0,
               100.0 * profile[i].bytes / total);
    }

    printf("\nTotal: %.2f MB\n", total / (1024.0 * 1024.0));
}
```

## Memory Budget Example

```c
/* Application with 100 MB memory limit */

typedef struct Application {
    membound *pool;           /* 100 MB bounded pool */

    /* Allocate from pool */
    multimap *users;          /* 20 MB - user cache */
    multilist *logs;          /* 30 MB - compressed logs */
    multiarray *events;       /* 10 MB - event buffer */
    intset *active_sessions;  /* 2 MB - session IDs */
    xofWriter *sensor_data;   /* 8 MB - compressed sensors */

    /* Remaining: 30 MB for application data */
} Application;

Application *appCreate(void) {
    Application *app = malloc(sizeof(*app));

    /* Create bounded pool */
    app->pool = memboundCreate(100 * 1024 * 1024);

    /* Allocate from pool with limits */
    app->users = memboundMalloc(app->pool, 20 * 1024 * 1024);
    app->logs = memboundMalloc(app->pool, 30 * 1024 * 1024);
    app->events = memboundMalloc(app->pool, 10 * 1024 * 1024);
    app->active_sessions = memboundMalloc(app->pool, 2 * 1024 * 1024);
    app->sensor_data = memboundMalloc(app->pool, 8 * 1024 * 1024);

    /* Remaining 30 MB available for dynamic use */

    return app;
}
```

## Memory Optimization Checklist

- [ ] **Use appropriate size variant** (Small for small data, not Full)
- [ ] **Enable compression** for archival data (multilist compress > 0)
- [ ] **Deduplicate strings** with multimapAtom when repeated
- [ ] **Use Native arrays** for fixed-size datasets
- [ ] **Choose compact string** (mdsc vs mds) when refcount not needed
- [ ] **Select optimal encoding** (intset INT16 vs INT32 vs INT64)
- [ ] **Compress time-series** with xof for doubles, dod for integers
- [ ] **Use float16/bfloat16** for graphics/ML when precision allows
- [ ] **Batch operations** to reduce allocation overhead
- [ ] **Set memory bounds** with membound for predictable usage
- [ ] **Profile regularly** to identify memory hotspots
- [ ] **Preallocate** when final size is known

## See Also

- [SCALE_AWARE.md](SCALE_AWARE.md) - Choosing the right size variant
- [PERFORMANCE.md](PERFORMANCE.md) - Performance vs memory trade-offs
- [membound](../modules/memory/MEMBOUND.md) - Bounded memory allocator
- [fibbuf](../modules/memory/FIBBUF.md) - Fibonacci buffer sizing
- [jebuf](../modules/memory/JEBUF.md) - jemalloc-aligned buffers
- [XOF](../modules/compression/XOF.md) - XOR filter compression
- [FLOAT16](../modules/compression/FLOAT16.md) - Half-precision floats
