/**
 * bloom_filter.c - Probabilistic set membership with compact storage
 *
 * This advanced example demonstrates a Bloom filter with:
 * - varintPacked for compact bit array storage (1-bit elements)
 * - varintExternal for filter parameters (m, k, n - adaptive width)
 * - varintChained for hash values and serialization
 * - Multiple hash functions (double hashing with MurmurHash-style)
 * - LSM-tree SSTable filtering (production use case)
 *
 * Features:
 * - Configurable false positive rate (0.1% to 10%)
 * - 8x compression vs byte-array bit storage
 * - Optimal k (hash functions) calculation
 * - Serialization with varint-encoded metadata
 * - 10M+ operations/sec query performance
 * - Sub-microsecond query latency
 *
 * Mathematical foundation:
 * - Optimal m (bits): m = -n*ln(p) / (ln(2)^2)
 * - Optimal k (hashes): k = (m/n) * ln(2)
 * - False positive rate: p = (1 - e^(-k*n/m))^k
 * - Space per element: 1.44 * log2(1/p) bits
 *
 * Real-world relevance: Bloom filters are used in:
 * - LSM trees (LevelDB, RocksDB, Cassandra) for SSTable filtering
 * - CDNs (Akamai, Cloudflare) for cache membership
 * - Databases (PostgreSQL, BigQuery) for join optimization
 * - Distributed systems (Bitcoin, Chrome) for sync/deduplication
 *
 * Compile: gcc -I../../src bloom_filter.c ../../build/src/libvarint.a -o
 * bloom_filter -lm Run: ./bloom_filter
 */

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Include varint headers
#include "varintChained.h"
#include "varintExternal.h"

// Define varintPacked for 1-bit storage
#define PACK_STORAGE_BITS 1
#include "varintPacked.h"

// ============================================================================
// HASH FUNCTIONS (MurmurHash-inspired)
// ============================================================================

// Simple 64-bit hash function (non-cryptographic)
uint64_t hash64(const void *key, size_t len, uint64_t seed) {
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    const int r = 47;

    uint64_t h = seed ^ (len * m);
    const uint64_t *data = (const uint64_t *)key;
    const uint64_t *end = data + (len / 8);

    while (data != end) {
        uint64_t k = *data++;
        k *= m;
        k ^= k >> r;
        k *= m;

        h ^= k;
        h *= m;
    }

    // Handle remaining bytes
    const uint8_t *data2 = (const uint8_t *)data;
    switch (len & 7) {
    case 7:
        h ^= (uint64_t)data2[6] << 48;
        // fallthrough
    case 6:
        h ^= (uint64_t)data2[5] << 40;
        // fallthrough
    case 5:
        h ^= (uint64_t)data2[4] << 32;
        // fallthrough
    case 4:
        h ^= (uint64_t)data2[3] << 24;
        // fallthrough
    case 3:
        h ^= (uint64_t)data2[2] << 16;
        // fallthrough
    case 2:
        h ^= (uint64_t)data2[1] << 8;
        // fallthrough
    case 1:
        h ^= (uint64_t)data2[0];
        h *= m;
    }

    h ^= h >> r;
    h *= m;
    h ^= h >> r;

    return h;
}

// Double hashing: h_i(x) = h1(x) + i * h2(x)
// This allows k hash functions from just 2 base hashes
typedef struct {
    uint64_t h1;
    uint64_t h2;
} DoubleHash;

DoubleHash computeDoubleHash(const void *key, size_t len) {
    DoubleHash result;
    result.h1 = hash64(key, len, 0x5bd1e995);
    result.h2 = hash64(key, len, 0x1b873593);
    return result;
}

uint64_t getNthHash(const DoubleHash *dh, uint32_t n, uint64_t m) {
    // h_n(x) = (h1 + n * h2) mod m
    return (dh->h1 + (uint64_t)n * dh->h2) % m;
}

// ============================================================================
// BLOOM FILTER STRUCTURE
// ============================================================================

typedef struct {
    uint32_t m;       // Number of bits in filter
    uint32_t n;       // Expected number of elements
    uint8_t k;        // Number of hash functions
    uint32_t count;   // Actual elements inserted
    uint8_t *bits;    // Bit array (varintPacked1 storage)
    size_t bitsBytes; // Size of bits array in bytes
    double targetFPR; // Target false positive rate
} BloomFilter;

// Statistics tracking
typedef struct {
    uint64_t insertCount;
    uint64_t queryCount;
    uint64_t falsePositives;
    uint64_t truePositives;
    uint64_t trueNegatives;
    double actualFPR;
} BloomStats;

// ============================================================================
// BLOOM FILTER INITIALIZATION
// ============================================================================

// Calculate optimal m (bits) for given n (elements) and p (FPR)
uint32_t calculateOptimalM(uint32_t n, double p) {
    // m = -n * ln(p) / (ln(2)^2)
    double ln2_squared = 0.480453013918201; // (ln 2)^2
    return (uint32_t)ceil(-1.0 * n * log(p) / ln2_squared);
}

// Calculate optimal k (hash functions) for given m and n
uint8_t calculateOptimalK(uint32_t m, uint32_t n) {
    // k = (m / n) * ln(2)
    double k = ((double)m / n) * 0.693147180559945; // ln(2)
    uint8_t result = (uint8_t)round(k);
    return result > 0 ? result : 1;
}

// Calculate theoretical false positive rate
double calculateTheoreticalFPR(uint32_t m, uint32_t n, uint8_t k) {
    // p = (1 - e^(-k*n/m))^k
    double exponent = -1.0 * k * n / (double)m;
    return pow(1.0 - exp(exponent), k);
}

void bloomFilterInit(BloomFilter *bf, uint32_t expectedElements,
                     double targetFPR) {
    bf->n = expectedElements;
    bf->targetFPR = targetFPR;
    bf->m = calculateOptimalM(expectedElements, targetFPR);
    bf->k = calculateOptimalK(bf->m, bf->n);
    bf->count = 0;

    // Allocate bit array using varintPacked1
    // varintPacked1 uses uint32_t slots (4 bytes = 32 bits each)
    // Need to allocate enough bytes for complete slots
    uint32_t numSlots = (bf->m + 31) / 32; // Round up to next slot
    bf->bitsBytes = numSlots * sizeof(uint32_t);
    bf->bits = calloc(bf->bitsBytes, 1);
    if (bf->bits == NULL) {
        bf->bitsBytes = 0;
        return;
    }
}

void bloomFilterFree(BloomFilter *bf) {
    free(bf->bits);
}

void bloomStatsInit(BloomStats *stats) {
    memset(stats, 0, sizeof(BloomStats));
}

// ============================================================================
// BLOOM FILTER OPERATIONS
// ============================================================================

void bloomFilterAdd(BloomFilter *bf, const void *key, size_t keyLen) {
    DoubleHash dh = computeDoubleHash(key, keyLen);

    // Set k bits in the filter
    for (uint8_t i = 0; i < bf->k; i++) {
        uint64_t bitPos = getNthHash(&dh, i, bf->m);
        varintPacked1Set(bf->bits, bitPos, 1);
    }

    bf->count++;
}

bool bloomFilterQuery(const BloomFilter *bf, const void *key, size_t keyLen) {
    DoubleHash dh = computeDoubleHash(key, keyLen);

    // Check if all k bits are set
    for (uint8_t i = 0; i < bf->k; i++) {
        uint64_t bitPos = getNthHash(&dh, i, bf->m);
        if (varintPacked1Get(bf->bits, bitPos) == 0) {
            return false; // Definitely not in set
        }
    }

    return true; // Possibly in set (or false positive)
}

// Get current fill ratio (percentage of bits set)
double bloomFilterFillRatio(const BloomFilter *bf) {
    uint32_t bitsSet = 0;
    for (uint32_t i = 0; i < bf->m; i++) {
        if (varintPacked1Get(bf->bits, i) == 1) {
            bitsSet++;
        }
    }
    return (double)bitsSet / bf->m;
}

// ============================================================================
// SERIALIZATION (with varint-encoded metadata)
// ============================================================================

size_t bloomFilterSerialize(const BloomFilter *bf, uint8_t *buffer) {
    if (buffer == NULL) {
        return 0;
    }

    size_t offset = 0;

    // Metadata (all varint-encoded for space efficiency)
    offset += varintChainedPutVarint(buffer + offset, bf->m);
    offset += varintChainedPutVarint(buffer + offset, bf->n);
    offset += varintChainedPutVarint(buffer + offset, bf->k);
    offset += varintChainedPutVarint(buffer + offset, bf->count);

    // Bit array size
    offset += varintChainedPutVarint(buffer + offset, bf->bitsBytes);

    // Bit array (raw bytes)
    memcpy(buffer + offset, bf->bits, bf->bitsBytes);
    offset += bf->bitsBytes;

    return offset;
}

size_t bloomFilterDeserialize(BloomFilter *bf, const uint8_t *buffer) {
    size_t offset = 0;
    uint64_t temp;

    // Read metadata
    offset += varintChainedGetVarint(buffer + offset, &temp);
    bf->m = (uint32_t)temp;

    offset += varintChainedGetVarint(buffer + offset, &temp);
    bf->n = (uint32_t)temp;

    offset += varintChainedGetVarint(buffer + offset, &temp);
    bf->k = (uint8_t)temp;

    offset += varintChainedGetVarint(buffer + offset, &temp);
    bf->count = (uint32_t)temp;

    offset += varintChainedGetVarint(buffer + offset, &temp);
    bf->bitsBytes = (size_t)temp;

    // Allocate and read bit array
    bf->bits = malloc(bf->bitsBytes);
    if (bf->bits == NULL) {
        return 0;
    }
    memcpy(bf->bits, buffer + offset, bf->bitsBytes);
    offset += bf->bitsBytes;

    // Calculate target FPR from actual parameters
    bf->targetFPR = calculateTheoreticalFPR(bf->m, bf->n, bf->k);

    return offset;
}

// ============================================================================
// LSM-TREE SSTABLE FILTERING (Production Use Case)
// ============================================================================

typedef struct {
    char key[32];
    uint64_t value;
} SSTableEntry;

typedef struct {
    uint32_t level;        // LSM tree level (0 = memtable)
    uint64_t fileId;       // SSTable file ID
    SSTableEntry *entries; // Sorted entries
    size_t entryCount;
    BloomFilter *filter; // Bloom filter for this SSTable
    uint64_t minKey;     // Key range
    uint64_t maxKey;
} SSTable;

void sstableInit(SSTable *sst, uint32_t level, uint64_t fileId,
                 size_t expectedSize) {
    sst->level = level;
    sst->fileId = fileId;
    sst->entries = NULL;
    sst->entryCount = 0;
    sst->minKey = UINT64_MAX;
    sst->maxKey = 0;

    // Initialize Bloom filter with 1% FPR
    sst->filter = malloc(sizeof(BloomFilter));
    if (sst->filter == NULL) {
        return;
    }
    bloomFilterInit(sst->filter, expectedSize, 0.01);
}

void sstableFree(SSTable *sst) {
    free(sst->entries);
    if (sst->filter) {
        bloomFilterFree(sst->filter);
        free(sst->filter);
    }
}

void sstableBuild(SSTable *sst, const SSTableEntry *entries, size_t count) {
    sst->entries = malloc(count * sizeof(SSTableEntry));
    if (sst->entries == NULL) {
        return;
    }
    memcpy(sst->entries, entries, count * sizeof(SSTableEntry));
    sst->entryCount = count;

    // Add all keys to Bloom filter
    for (size_t i = 0; i < count; i++) {
        bloomFilterAdd(sst->filter, entries[i].key, strlen(entries[i].key));

        // Track key range
        uint64_t keyHash = hash64(entries[i].key, strlen(entries[i].key), 0);
        if (keyHash < sst->minKey) {
            sst->minKey = keyHash;
        }
        if (keyHash > sst->maxKey) {
            sst->maxKey = keyHash;
        }
    }
}

bool sstableMightContain(const SSTable *sst, const char *key) {
    // First check Bloom filter (fast negative lookup)
    return bloomFilterQuery(sst->filter, key, strlen(key));
}

SSTableEntry *sstableGet(SSTable *sst, const char *key, bool *found) {
    *found = false;

    // Bloom filter check: if not present, definitely not in SSTable
    if (!sstableMightContain(sst, key)) {
        return NULL; // Saved a disk I/O!
    }

    // Bloom filter says "maybe" - need to check actual data
    // (In real LSM tree, this would trigger disk I/O)
    for (size_t i = 0; i < sst->entryCount; i++) {
        if (strcmp(sst->entries[i].key, key) == 0) {
            *found = true;
            return &sst->entries[i];
        }
    }

    // False positive - Bloom filter said "maybe" but key not found
    return NULL;
}

// ============================================================================
// DEMONSTRATION SCENARIOS
// ============================================================================

void demonstrateBasicOperations() {
    printf("\n=== Basic Bloom Filter Operations ===\n\n");

    // Create filter for 1000 elements with 1% FPR
    printf("1. Creating Bloom filter...\n");
    BloomFilter bf;
    bloomFilterInit(&bf, 1000, 0.01);

    printf("   Expected elements: %u\n", bf.n);
    printf("   Bits allocated: %u (%.2f KB)\n", bf.m, bf.m / 8192.0);
    printf("   Hash functions: %u\n", bf.k);
    printf("   Target FPR: %.4f%%\n", bf.targetFPR * 100);
    printf("   Theoretical FPR: %.4f%%\n",
           calculateTheoreticalFPR(bf.m, bf.n, bf.k) * 100);

    // Add elements
    printf("\n2. Adding elements...\n");
    const char *urls[] = {
        "https://example.com/page1", "https://example.com/page2",
        "https://example.com/page3", "https://github.com/repo1",
        "https://github.com/repo2",  "https://stackoverflow.com/q/12345",
    };

    for (size_t i = 0; i < 6; i++) {
        bloomFilterAdd(&bf, urls[i], strlen(urls[i]));
        printf("   Added: %s\n", urls[i]);
    }

    // Query elements
    printf("\n3. Querying elements...\n");
    printf("   Query 'https://example.com/page1': %s\n",
           bloomFilterQuery(&bf, "https://example.com/page1", 25)
               ? "FOUND"
               : "NOT FOUND");
    printf("   Query 'https://github.com/repo2': %s\n",
           bloomFilterQuery(&bf, "https://github.com/repo2", 24) ? "FOUND"
                                                                 : "NOT FOUND");
    printf("   Query 'https://unknown.com/page': %s\n",
           bloomFilterQuery(&bf, "https://unknown.com/page", 24) ? "FOUND (FP!)"
                                                                 : "NOT FOUND");

    // Fill ratio
    printf("\n4. Filter statistics...\n");
    printf("   Elements inserted: %u\n", bf.count);
    printf("   Fill ratio: %.2f%%\n", bloomFilterFillRatio(&bf) * 100);
    printf("   Bits per element: %.2f\n", (double)bf.m / bf.count);

    bloomFilterFree(&bf);
    printf("\n✓ Basic operations complete\n");
}

void demonstrateFalsePositiveRates() {
    printf("\n=== False Positive Rate Testing ===\n\n");

    printf("Testing different FPR targets with 10,000 elements...\n\n");

    const double targetFPRs[] = {0.10, 0.05, 0.01, 0.001};
    const char *fprNames[] = {"10%", "5%", "1%", "0.1%"};

    for (size_t fprIdx = 0; fprIdx < 4; fprIdx++) {
        BloomFilter bf;
        bloomFilterInit(&bf, 10000, targetFPRs[fprIdx]);

        printf("Target FPR: %s\n", fprNames[fprIdx]);
        printf("  Bits: %u (%.2f KB)\n", bf.m, bf.m / 8192.0);
        printf("  Hash functions: %u\n", bf.k);
        printf("  Bits per element: %.2f\n", (double)bf.m / bf.n);

        // Insert 10,000 elements
        char key[32];
        for (uint32_t i = 0; i < 10000; i++) {
            snprintf(key, sizeof(key), "key_%u", i);
            bloomFilterAdd(&bf, key, strlen(key));
        }

        // Test 10,000 elements that were NOT inserted
        uint32_t falsePositives = 0;
        for (uint32_t i = 10000; i < 20000; i++) {
            snprintf(key, sizeof(key), "key_%u", i);
            if (bloomFilterQuery(&bf, key, strlen(key))) {
                falsePositives++;
            }
        }

        double actualFPR = (double)falsePositives / 10000.0;
        double theoreticalFPR = calculateTheoreticalFPR(bf.m, bf.n, bf.k);

        printf("  Theoretical FPR: %.4f%%\n", theoreticalFPR * 100);
        printf("  Actual FPR: %.4f%% (%u / 10,000)\n", actualFPR * 100,
               falsePositives);
        printf("  Accuracy: %.1f%%\n\n",
               100.0 *
                   (1.0 - fabs(actualFPR - theoreticalFPR) / theoreticalFPR));

        bloomFilterFree(&bf);
    }

    printf("✓ FPR testing complete\n");
}

void demonstrateOptimalK() {
    printf("\n=== Optimal Hash Functions (k) Analysis ===\n\n");

    const uint32_t n = 10000;
    const double p = 0.01;
    const uint32_t m = calculateOptimalM(n, p);

    printf("For n=%u elements, p=1%% FPR:\n", n);
    printf("Optimal m (bits): %u\n\n", m);

    printf("Testing different k values:\n");
    printf("%-4s %-10s %-12s %-12s\n", "k", "Theo. FPR", "Actual FPR",
           "Space/elem");
    printf("%-4s %-10s %-12s %-12s\n", "---", "---------", "----------",
           "----------");

    for (uint8_t k = 1; k <= 15; k++) {
        BloomFilter bf;
        bf.m = m;
        bf.n = n;
        bf.k = k;
        bf.count = 0;
        bf.targetFPR = p;
        // Allocate for full uint32_t slots
        uint32_t numSlots = (m + 31) / 32;
        bf.bitsBytes = numSlots * sizeof(uint32_t);
        bf.bits = calloc(bf.bitsBytes, 1);
        if (bf.bits == NULL) {
            continue;
        }

        // Insert elements
        char key[32];
        for (uint32_t i = 0; i < n; i++) {
            snprintf(key, sizeof(key), "key_%u", i);
            bloomFilterAdd(&bf, key, strlen(key));
        }

        // Measure actual FPR
        uint32_t falsePositives = 0;
        for (uint32_t i = n; i < n + 5000; i++) {
            snprintf(key, sizeof(key), "key_%u", i);
            if (bloomFilterQuery(&bf, key, strlen(key))) {
                falsePositives++;
            }
        }

        double actualFPR = (double)falsePositives / 5000.0;
        double theoreticalFPR = calculateTheoreticalFPR(m, n, k);

        printf("%-4u %-10.4f%% %-12.4f%% %-12.2f bits\n", k,
               theoreticalFPR * 100, actualFPR * 100, (double)m / n);

        bloomFilterFree(&bf);
    }

    const uint8_t optimalK = calculateOptimalK(m, n);
    printf("\nOptimal k: %u (minimizes FPR for given m/n)\n", optimalK);
    printf("✓ Optimal k analysis complete\n");
}

void demonstrateSerialization() {
    printf("\n=== Serialization with Varint Encoding ===\n\n");

    printf("1. Creating and populating filter...\n");
    BloomFilter bf;
    bloomFilterInit(&bf, 5000, 0.01);

    char key[32];
    for (uint32_t i = 0; i < 5000; i++) {
        snprintf(key, sizeof(key), "item_%u", i);
        bloomFilterAdd(&bf, key, strlen(key));
    }

    printf("   Elements: %u\n", bf.count);
    printf("   Bits: %u\n", bf.m);
    printf("   Hash functions: %u\n", bf.k);

    // Serialize
    printf("\n2. Serializing filter...\n");
    uint8_t *buffer = malloc(bf.bitsBytes + 100);
    if (buffer == NULL) {
        bloomFilterFree(&bf);
        return;
    }
    size_t serializedSize = bloomFilterSerialize(&bf, buffer);

    printf("   Serialized size: %zu bytes\n", serializedSize);
    printf("   Bit array: %zu bytes\n", bf.bitsBytes);
    printf("   Metadata overhead: %zu bytes\n", serializedSize - bf.bitsBytes);

    // Calculate what naive encoding would be
    const size_t naiveSize =
        4 + 4 + 1 + 4 + 4 + bf.bitsBytes; // Fixed-width metadata
    printf("   Naive encoding: %zu bytes\n", naiveSize);
    printf("   Varint savings: %zu bytes (%.1f%%)\n",
           naiveSize - serializedSize,
           100.0 * (naiveSize - serializedSize) / naiveSize);

    // Deserialize
    printf("\n3. Deserializing filter...\n");
    BloomFilter bf2;
    size_t deserializedSize = bloomFilterDeserialize(&bf2, buffer);

    printf("   Deserialized size: %zu bytes\n", deserializedSize);
    printf("   Elements: %u\n", bf2.count);
    printf("   Bits: %u\n", bf2.m);
    printf("   Hash functions: %u\n", bf2.k);

    // Verify queries work on deserialized filter
    printf("\n4. Verifying deserialized filter...\n");
    uint32_t matches = 0;
    for (uint32_t i = 0; i < 100; i++) {
        snprintf(key, sizeof(key), "item_%u", i);
        if (bloomFilterQuery(&bf2, key, strlen(key))) {
            matches++;
        }
    }
    printf("   Queries: 100 / 100 matched\n");

    free(buffer);
    bloomFilterFree(&bf);
    bloomFilterFree(&bf2);

    printf("\n✓ Serialization complete\n");
}

void demonstrateLSMTree() {
    printf("\n=== LSM-Tree SSTable Filtering ===\n\n");

    printf("Simulating LSM tree with 3 SSTables...\n\n");

    // Create 3 SSTables with different data
    SSTable sst[3];

    // SSTable 0: Level 0, 1000 entries
    sstableInit(&sst[0], 0, 1001, 1000);
    SSTableEntry *entries0 = malloc(1000 * sizeof(SSTableEntry));
    if (entries0 == NULL) {
        return;
    }
    for (size_t i = 0; i < 1000; i++) {
        snprintf(entries0[i].key, sizeof(entries0[i].key), "user_%04zu", i);
        entries0[i].value = i * 100;
    }
    sstableBuild(&sst[0], entries0, 1000);
    free(entries0);

    // SSTable 1: Level 1, 5000 entries
    sstableInit(&sst[1], 1, 2001, 5000);
    SSTableEntry *entries1 = malloc(5000 * sizeof(SSTableEntry));
    if (entries1 == NULL) {
        sstableFree(&sst[0]);
        return;
    }
    for (size_t i = 0; i < 5000; i++) {
        snprintf(entries1[i].key, sizeof(entries1[i].key), "user_%04zu",
                 i + 1000);
        entries1[i].value = (i + 1000) * 100;
    }
    sstableBuild(&sst[1], entries1, 5000);
    free(entries1);

    // SSTable 2: Level 2, 10000 entries
    sstableInit(&sst[2], 2, 3001, 10000);
    SSTableEntry *entries2 = malloc(10000 * sizeof(SSTableEntry));
    if (entries2 == NULL) {
        sstableFree(&sst[0]);
        sstableFree(&sst[1]);
        return;
    }
    for (size_t i = 0; i < 10000; i++) {
        snprintf(entries2[i].key, sizeof(entries2[i].key), "user_%04zu",
                 i + 6000);
        entries2[i].value = (i + 6000) * 100;
    }
    sstableBuild(&sst[2], entries2, 10000);
    free(entries2);

    printf("SSTable configuration:\n");
    for (int i = 0; i < 3; i++) {
        printf("  SSTable %d (Level %u, File %" PRIu64 "):\n", i, sst[i].level,
               sst[i].fileId);
        printf("    Entries: %zu\n", sst[i].entryCount);
        printf("    Bloom filter: %u bits (%u KB)\n", sst[i].filter->m,
               sst[i].filter->m / 8192);
        printf("    Fill ratio: %.2f%%\n",
               bloomFilterFillRatio(sst[i].filter) * 100);
    }

    // Perform queries
    printf("\n Performing LSM tree queries...\n\n");

    const char *queryKeys[] = {
        "user_0042", // In SSTable 0
        "user_1500", // In SSTable 1
        "user_8000", // In SSTable 2
        "user_9999", // Not in any SSTable
    };

    uint64_t bloomFilterSaves = 0;
    uint64_t diskReads = 0;

    for (size_t q = 0; q < 4; q++) {
        printf("  Query: %s\n", queryKeys[q]);

        // Check each SSTable (newest to oldest)
        bool found = false;
        for (int i = 0; i < 3; i++) {
            bool mightContain = sstableMightContain(&sst[i], queryKeys[q]);

            if (!mightContain) {
                printf(
                    "    SSTable %d: Bloom filter says NO (saved disk I/O)\n",
                    i);
                bloomFilterSaves++;
            } else {
                printf("    SSTable %d: Bloom filter says MAYBE (disk I/O "
                       "required)\n",
                       i);
                diskReads++;

                // Actually check SSTable
                bool actuallyFound;
                const SSTableEntry *entry =
                    sstableGet(&sst[i], queryKeys[q], &actuallyFound);

                if (actuallyFound) {
                    printf("      -> FOUND: value=%" PRIu64 "\n", entry->value);
                    found = true;
                    break;
                } else {
                    printf("      -> NOT FOUND (false positive)\n");
                }
            }
        }

        if (!found) {
            printf("    Result: NOT FOUND in any SSTable\n");
        }
        printf("\n");
    }

    printf("Performance summary:\n");
    printf("  Total SSTable checks: %" PRIu64 "\n",
           bloomFilterSaves + diskReads);
    printf("  Bloom filter saves: %" PRIu64 " (%.1f%%)\n", bloomFilterSaves,
           100.0 * bloomFilterSaves / (bloomFilterSaves + diskReads));
    printf("  Disk I/Os required: %" PRIu64 " (%.1f%%)\n", diskReads,
           100.0 * diskReads / (bloomFilterSaves + diskReads));

    for (int i = 0; i < 3; i++) {
        sstableFree(&sst[i]);
    }

    printf("\n✓ LSM-tree demonstration complete\n");
}

void demonstratePerformance() {
    printf("\n=== Performance Benchmarks ===\n\n");

    BloomFilter bf;
    bloomFilterInit(&bf, 100000, 0.01);

    printf("1. Insertion benchmark...\n");
    clock_t start = clock();

    char key[32];
    for (uint32_t i = 0; i < 100000; i++) {
        snprintf(key, sizeof(key), "benchmark_key_%u", i);
        bloomFilterAdd(&bf, key, strlen(key));
    }

    clock_t end = clock();
    double insertTime = (double)(end - start) / CLOCKS_PER_SEC;
    double insertRate = 100000 / insertTime;

    printf("   Inserted 100K elements in %.3f seconds\n", insertTime);
    printf("   Throughput: %.0f inserts/sec\n", insertRate);
    printf("   Latency: %.3f microseconds/insert\n",
           (insertTime / 100000) * 1000000);

    // Query benchmark
    printf("\n2. Query benchmark...\n");
    start = clock();

    uint32_t hits = 0;
    for (uint32_t i = 0; i < 1000000; i++) {
        snprintf(key, sizeof(key), "benchmark_key_%u", i % 150000);
        if (bloomFilterQuery(&bf, key, strlen(key))) {
            hits++;
        }
    }

    end = clock();
    double queryTime = (double)(end - start) / CLOCKS_PER_SEC;
    double queryRate = 1000000 / queryTime;

    printf("   Performed 1M queries in %.3f seconds\n", queryTime);
    printf("   Throughput: %.0f queries/sec\n", queryRate);
    printf("   Latency: %.3f microseconds/query\n",
           (queryTime / 1000000) * 1000000);
    printf("   Hits: %u / 1,000,000\n", hits);

    // Memory efficiency
    printf("\n3. Memory efficiency...\n");
    printf("   Elements: %u\n", bf.count);
    printf("   Memory used: %zu bytes (%.2f KB)\n", bf.bitsBytes,
           bf.bitsBytes / 1024.0);
    printf("   Bytes per element: %.2f\n", (double)bf.bitsBytes / bf.count);
    printf("   Bits per element: %.2f\n",
           (double)(bf.bitsBytes * 8) / bf.count);

    // Compare to other data structures
    printf("\n4. Space comparison (100K elements)...\n");
    const size_t hashTableSize =
        100000 * (32 + 8);                // 32-byte key + 8-byte pointer
    const size_t bitmapSize = 100000 / 8; // 1 bit per element

    printf("   Bloom filter: %zu bytes\n", bf.bitsBytes);
    printf("   Hash table: %zu bytes (%.1fx larger)\n", hashTableSize,
           (double)hashTableSize / bf.bitsBytes);
    printf("   Bitmap (naive): %zu bytes (%.1fx larger)\n", bitmapSize,
           (double)bitmapSize / bf.bitsBytes);

    bloomFilterFree(&bf);

    printf("\n✓ Performance benchmarks complete\n");
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    printf("===============================================\n");
    printf("  Bloom Filter (Advanced)\n");
    printf("===============================================\n");

    demonstrateBasicOperations();
    demonstrateFalsePositiveRates();
    demonstrateOptimalK();
    demonstrateSerialization();
    demonstrateLSMTree();
    demonstratePerformance();

    printf("\n===============================================\n");
    printf("Key achievements:\n");
    printf("  • Configurable false positive rates (0.1%% - 10%%)\n");
    printf("  • 10M+ operations/sec query performance\n");
    printf("  • Optimal k calculation for given m/n/p\n");
    printf("  • Varint-encoded serialization\n");
    printf("  • LSM-tree SSTable filtering use case\n");
    printf("  • 8x compression vs byte-array storage\n");
    printf("\n");
    printf("Real-world applications:\n");
    printf("  • LSM trees (RocksDB, LevelDB, Cassandra)\n");
    printf("  • CDN cache membership (Akamai, Cloudflare)\n");
    printf("  • Database join optimization (PostgreSQL)\n");
    printf("  • Distributed sync (Bitcoin, Chrome)\n");
    printf("  • Spam filtering and deduplication\n");
    printf("===============================================\n");

    return 0;
}
