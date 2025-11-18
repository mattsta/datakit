# offsetArray - Sparse Integer-Indexed Arrays

## Overview

`offsetArray` provides **macro-based sparse arrays** that automatically adjust their base offset to avoid wasting memory on unused indices. When your integer indices don't start at zero, offsetArray lets you use them directly without allocating empty slots.

**Key Features:**
- Zero memory waste for non-zero-based indices
- Automatic offset adjustment as indices change
- Direct array access (no hash table overhead)
- Generic type support via C11 generics or macros
- Grows both upward (larger indices) and downward (smaller indices)
- Header-only implementation (macro-based)

**Header**: `offsetArray.h`

**Source**: `offsetArray.c` (test code only)

## What is an Offset Array?

Imagine you need an array indexed by port numbers 8000-8100:

### Traditional Approach (Wasteful)

```c
int *ports = malloc(8101 * sizeof(int));
// Allocates 8101 slots, wastes slots 0-7999!

ports[8000] = connectionsOnPort(8000);
ports[8080] = connectionsOnPort(8080);
// Using only 101 slots, wasted 7,999 slots (98.7% waste!)
```

### Hash Table (Overhead)

```c
// Need hash function, collision handling, etc.
// Slower access, more complex code
hashMap *ports = hashMapCreate();
hashMapSet(ports, 8000, connections8000);
```

### offsetArray Approach (Efficient)

```c
offsetArrayint arr = {0};

offsetArrayGrow(&arr, 8000);
offsetArrayGet(&arr, 8000) = connections8000;

offsetArrayGrow(&arr, 8080);
offsetArrayGet(&arr, 8080) = connections8080;

// Only allocates 81 slots (8000-8080)!
// Direct array access, O(1) performance
// Zero wasted space
```

## How It Works

offsetArray tracks three values:
1. **offset** - Lowest index in the array
2. **highest** - Highest index in the array
3. **obj** - Pointer to the actual array

```
Conceptual view:
    Index:  8000  8001  8002  ...  8080
    ┌─────┬─────┬─────┬─────┬─────┐
    │  0  │  1  │  2  │ ... │ 80  │  Actual array positions
    └─────┴─────┴─────┴─────┴─────┘

offset = 8000
highest = 8080
obj = malloc((8080 - 8000 + 1) * sizeof(int))

Access arr[8050]:
    realIndex = 8050 - offset
              = 8050 - 8000
              = 50
    return obj[50]
```

## API Reference

### Type Definition

```c
/* Define an offsetArray type for your storage type
 * name: Type suffix (e.g., "Int" for offsetArrayInt)
 * storage: Type to store (e.g., int, void*, struct foo)
 * scale: Integer type for indices (e.g., int, size_t, uint32_t)
 */
#define offsetArrayCreateTypes(name, storage, scale)

/* Example: Create type for storing integers indexed by size_t */
offsetArrayCreateTypes(Int, int, size_t);
// Creates type: offsetArrayInt

/* Example: Create type for storing pointers */
offsetArrayCreateTypes(Ptr, void*, int);
// Creates type: offsetArrayPtr

/* Example: Create type for storing custom structs */
typedef struct myData {
    int value;
    char name[32];
} myData;

offsetArrayCreateTypes(Data, myData, size_t);
// Creates type: offsetArrayData
```

### Growing the Array

```c
/* Ensure array can store value at index
 * arr: pointer to offsetArray
 * idx: index to make accessible
 * Effect: Reallocates array if necessary to include idx
 */
#define offsetArrayGrow(arr, idx)

/* Example */
offsetArrayInt arr = {0};  /* Initialize to zero */

offsetArrayGrow(&arr, 100);  /* Now can access arr[100] */
offsetArrayGrow(&arr, 200);  /* Grows to include 200 */
offsetArrayGrow(&arr, 50);   /* Grows DOWNWARD to include 50 */
```

### Accessing Elements

```c
/* Get element at index
 * arr: pointer to offsetArray
 * idx: index to access
 * Returns: lvalue reference to element
 */
#define offsetArrayGet(arr, idx)

/* Direct access by zero-based position
 * arr: pointer to offsetArray
 * zeroIdx: position in internal array (0 = first element)
 * Returns: lvalue reference to element
 */
#define offsetArrayDirect(arr, zeroIdx)

/* Example */
offsetArrayInt arr = {0};

offsetArrayGrow(&arr, 100);
offsetArrayGet(&arr, 100) = 42;  /* Set value */

int value = offsetArrayGet(&arr, 100);  /* Get value */
printf("Value at 100: %d\n", value);  /* 42 */

/* Direct access (bypass offset calculation) */
int firstValue = offsetArrayDirect(&arr, 0);  /* First element */
```

### Array Information

```c
/* Get number of elements in array
 * arr: pointer to offsetArray
 * Returns: highest - offset + 1
 */
#define offsetArrayCount(arr)

/* Example */
offsetArrayInt arr = {0};
offsetArrayGrow(&arr, 100);
offsetArrayGrow(&arr, 200);

printf("Elements: %zu\n", offsetArrayCount(&arr));
// Output: Elements: 101 (from 100 to 200 inclusive)
```

### Freeing Memory

```c
/* Free array memory
 * arr: pointer to offsetArray
 */
#define offsetArrayFree(arr)

/* Example */
offsetArrayInt arr = {0};
offsetArrayGrow(&arr, 100);
// ... use array ...
offsetArrayFree(&arr);
```

## Real-World Examples

### Example 1: Port Connection Tracking

```c
offsetArrayCreateTypes(Connections, int, int);

typedef struct portMonitor {
    offsetArrayConnections ports;
} portMonitor;

portMonitor *portMonitorCreate(void) {
    portMonitor *pm = malloc(sizeof(*pm));
    pm->ports = (offsetArrayConnections){0};
    return pm;
}

void portMonitorAdd(portMonitor *pm, int port, int connections) {
    offsetArrayGrow(&pm->ports, port);
    offsetArrayGet(&pm->ports, port) = connections;
}

int portMonitorGet(portMonitor *pm, int port) {
    /* Check if port is in range */
    if (pm->ports.offset == 0 || port < pm->ports.offset ||
        port > pm->ports.highest) {
        return -1;  /* Not monitored */
    }

    return offsetArrayGet(&pm->ports, port);
}

/* Usage */
portMonitor *pm = portMonitorCreate();

portMonitorAdd(pm, 8080, 100);  /* HTTP */
portMonitorAdd(pm, 8443, 50);   /* HTTPS */
portMonitorAdd(pm, 3306, 25);   /* MySQL */

printf("Port 8080: %d connections\n", portMonitorGet(pm, 8080));
printf("Port 3306: %d connections\n", portMonitorGet(pm, 3306));

offsetArrayFree(&pm->ports);
```

### Example 2: Sparse File Block Storage

```c
offsetArrayCreateTypes(BlockData, void*, size_t);

typedef struct sparseFile {
    offsetArrayBlockData blocks;
    size_t blockSize;
} sparseFile;

void sparseFileWrite(sparseFile *sf, size_t blockNum, void *data) {
    offsetArrayGrow(&sf->blocks, blockNum);

    /* Free old data if exists */
    void *old = offsetArrayGet(&sf->blocks, blockNum);
    if (old) {
        free(old);
    }

    /* Allocate and copy new data */
    void *newBlock = malloc(sf->blockSize);
    memcpy(newBlock, data, sf->blockSize);
    offsetArrayGet(&sf->blocks, blockNum) = newBlock;
}

void *sparseFileRead(sparseFile *sf, size_t blockNum) {
    if (blockNum < sf->blocks.offset || blockNum > sf->blocks.highest) {
        return NULL;  /* Block not allocated */
    }

    return offsetArrayGet(&sf->blocks, blockNum);
}

/* Usage: File with 4KB blocks */
sparseFile sf = {
    .blocks = {0},
    .blockSize = 4096
};

char data1[4096] = "Block 1000 data";
char data2[4096] = "Block 5000 data";

sparseFileWrite(&sf, 1000, data1);  /* Only allocates one block */
sparseFileWrite(&sf, 5000, data2);  /* Grows array, but no blocks 1001-4999! */

// Array internally: blocks[0] = data at 1000
//                   blocks[1-3999] = NULL
//                   blocks[4000] = data at 5000
// Actual memory: Only ~32 KB (2 blocks + small array overhead)
// vs full array: ~20 MB for blocks 0-5000!
```

### Example 3: Process ID Tracking

```c
offsetArrayCreateTypes(ProcInfo, struct procInfo*, int);

typedef struct procInfo {
    char name[256];
    time_t startTime;
    size_t memoryUsed;
} procInfo;

typedef struct procManager {
    offsetArrayProcInfo procs;
} procManager;

void procManagerTrack(procManager *pm, pid_t pid) {
    offsetArrayGrow(&pm->procs, pid);

    procInfo *info = malloc(sizeof(*info));
    /* ... populate info ... */

    offsetArrayGet(&pm->procs, pid) = info;
}

procInfo *procManagerGet(procManager *pm, pid_t pid) {
    if (pm->procs.offset == 0 ||
        pid < pm->procs.offset ||
        pid > pm->procs.highest) {
        return NULL;
    }

    return offsetArrayGet(&pm->procs, pid);
}

/* Usage */
procManager pm = { .procs = {0} };

/* PIDs are typically in range 1000-30000 on Linux */
procManagerTrack(&pm, 1234);
procManagerTrack(&pm, 5678);
procManagerTrack(&pm, 25000);

/* Array only spans 1234-25000, not 0-25000! */
printf("Array size: %zu entries\n", offsetArrayCount(&pm.procs));
// Output: Array size: 23767 entries (not 25001)
```

### Example 4: Year-Based Statistics

```c
offsetArrayCreateTypes(Stats, struct yearStats, int);

typedef struct yearStats {
    size_t revenue;
    size_t expenses;
    size_t profit;
} yearStats;

offsetArrayStats companyData = {0};

/* Start tracking from year 2000 */
offsetArrayGrow(&companyData, 2000);
offsetArrayGet(&companyData, 2000) = (yearStats){
    .revenue = 1000000,
    .expenses = 800000,
    .profit = 200000
};

/* Add 2023 data */
offsetArrayGrow(&companyData, 2023);
offsetArrayGet(&companyData, 2023) = (yearStats){
    .revenue = 5000000,
    .expenses = 3000000,
    .profit = 2000000
};

/* No memory wasted for years 0-1999! */
printf("Tracking years %d-%d\n",
       companyData.offset, companyData.highest);
// Output: Tracking years 2000-2023

/* Iterate over all years */
for (int year = companyData.offset; year <= companyData.highest; year++) {
    yearStats *stats = &offsetArrayGet(&companyData, year);
    printf("%d: Profit = %zu\n", year, stats->profit);
}
```

## Performance Characteristics

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Create | O(1) | Just zero initialization |
| Grow (extend) | O(n) | n = new array size, realloc + possible copy |
| Grow (prepend) | O(n) | Must memmove existing data |
| Get/Set | O(1) | Simple offset subtraction + array access |
| Count | O(1) | Simple calculation |
| Free | O(1) | Single free call |

### Growth Patterns

```c
/* Growing upward: O(n) - realloc may copy */
offsetArrayGrow(&arr, 100);  /* Allocate 1 slot */
offsetArrayGrow(&arr, 200);  /* Realloc to 101 slots */

/* Growing downward: O(n) - must memmove */
offsetArrayGrow(&arr, 50);
/* Steps:
   1. Realloc to 151 slots
   2. memmove old data 50 slots forward
   3. Update offset
*/
```

**Best practice**: If you know the range, grow once to bounds:

```c
/* Suboptimal - many reallocs */
for (int i = 100; i <= 200; i++) {
    offsetArrayGrow(&arr, i);
    offsetArrayGet(&arr, i) = i;
}

/* Better - grow once */
offsetArrayGrow(&arr, 100);  /* Set lower bound */
offsetArrayGrow(&arr, 200);  /* Set upper bound */
for (int i = 100; i <= 200; i++) {
    offsetArrayGet(&arr, i) = i;
}
```

## Memory Efficiency

### Comparison with Alternatives

```c
/* Problem: Store data for indices 1000-1100 (101 values) */

/* Approach 1: Regular array - WASTEFUL */
int *arr1 = calloc(1101, sizeof(int));
// Memory: 1101 × 4 = 4,404 bytes
// Waste: 1000 unused slots (90.8% waste!)

/* Approach 2: Hash table */
hashTable *ht = hashTableCreate();
// Memory: ~101 × (16 bytes overhead + 4 bytes value) = ~2,020 bytes
// Overhead: Hash function, collision handling, no direct indexing

/* Approach 3: offsetArray - OPTIMAL */
offsetArrayInt arr3 = {0};
offsetArrayGrow(&arr3, 1000);
offsetArrayGrow(&arr3, 1100);
// Memory: 101 × 4 = 404 bytes + 24 bytes metadata = 428 bytes
// No waste, direct indexing!
```

## Best Practices

### 1. Initialize to Zero

```c
/* GOOD - proper initialization */
offsetArrayInt arr = {0};

/* BAD - uninitialized */
offsetArrayInt arr;
offsetArrayGrow(&arr, 100);  /* Undefined behavior! */
```

### 2. Grow to Bounds First

```c
/* GOOD - grow once to range */
offsetArrayGrow(&arr, minIndex);
offsetArrayGrow(&arr, maxIndex);
for (int i = minIndex; i <= maxIndex; i++) {
    offsetArrayGet(&arr, i) = data[i];
}

/* BAD - grows every iteration */
for (int i = minIndex; i <= maxIndex; i++) {
    offsetArrayGrow(&arr, i);  /* Reallocates every time! */
    offsetArrayGet(&arr, i) = data[i];
}
```

### 3. Check Bounds Before Access

```c
/* GOOD - validate index */
if (idx >= arr.offset && idx <= arr.highest) {
    int value = offsetArrayGet(&arr, idx);
} else {
    printf("Index out of range\n");
}

/* BAD - no bounds checking */
int value = offsetArrayGet(&arr, idx);  /* May crash! */
```

### 4. Free When Done

```c
/* GOOD - cleanup */
offsetArrayInt arr = {0};
// ... use array ...
offsetArrayFree(&arr);

/* BAD - memory leak */
offsetArrayInt arr = {0};
// ... use array ...
// Never freed!
```

## Common Pitfalls

### 1. Forgetting to Grow

```c
/* WRONG - never grew array */
offsetArrayInt arr = {0};
offsetArrayGet(&arr, 100) = 42;  /* CRASH - arr.obj is NULL! */

/* RIGHT - grow first */
offsetArrayInt arr = {0};
offsetArrayGrow(&arr, 100);
offsetArrayGet(&arr, 100) = 42;  /* Safe */
```

### 2. Using After Free

```c
/* WRONG */
offsetArrayInt arr = {0};
offsetArrayGrow(&arr, 100);
offsetArrayFree(&arr);
int val = offsetArrayGet(&arr, 100);  /* Use after free! */

/* RIGHT */
offsetArrayInt arr = {0};
offsetArrayGrow(&arr, 100);
int val = offsetArrayGet(&arr, 100);
offsetArrayFree(&arr);  /* Free last */
```

### 3. Negative Indices with Unsigned Scale

```c
/* WRONG - unsigned can't represent negative */
offsetArrayCreateTypes(Bad, int, size_t);
offsetArrayBad arr = {0};
offsetArrayGrow(&arr, -5);  /* Wraps to huge positive! */

/* RIGHT - use signed scale for negative indices */
offsetArrayCreateTypes(Good, int, int);
offsetArrayGood arr = {0};
offsetArrayGrow(&arr, -5);  /* Works correctly */
```

### 4. Not Accounting for Growth Cost

```c
/* WRONG - expensive for large ranges */
for (int i = 0; i < 1000000; i += 100) {
    offsetArrayGrow(&arr, i);  /* 10,000 grows! */
}

/* RIGHT - grow once to final range */
offsetArrayGrow(&arr, 0);
offsetArrayGrow(&arr, 1000000);  /* 2 grows total */
```

## When NOT to Use offsetArray

1. **Continuous indices starting at 0** - Use regular array
2. **Very sparse data** (< 1% density) - Use hash table
3. **Unknown index range** - Use hash table or dynamic structure
4. **Need fast prepend operations** - Prepending is O(n) due to memmove
5. **Thread-safe access** - offsetArray has no locking

## Comparison Table

| Data Structure | Access Time | Memory | Insert Time | Use Case |
|----------------|-------------|--------|-------------|----------|
| Regular array | O(1) | Wasteful | O(1) | Dense, zero-based |
| offsetArray | O(1) | Efficient | O(n) grow | Sparse, non-zero |
| Hash table | O(1) avg | Overhead | O(1) avg | Very sparse |
| Binary search tree | O(log n) | Efficient | O(log n) | Ordered iteration |

## See Also

- [ptrPrevNext](PTR_PREV_NEXT.md) - Compact linked list pointer storage
- [membound](MEMBOUND.md) - Bounded memory allocator

## Testing

Run the offsetArray test suite:

```bash
./src/datakit-test test offsetArray
```

The test suite validates:
- Growing upward (increasing indices)
- Growing downward (decreasing indices)
- Random access patterns
- Large index ranges (0-8192)
- Memory correctness with valgrind
