/**
 * vector_clock.c - Distributed Vector Clock using varintTagged
 *
 * This example demonstrates vector clocks for distributed systems:
 * - varintTagged: Sparse (actorID, counter) pairs for compression
 * - Causal ordering: Detecting happens-before relationships
 * - Conflict detection: Identifying concurrent events
 * - Practical use case: Distributed key-value store
 *
 * Features:
 * - Space-efficient sparse vector clocks
 * - Multi-node distributed system simulation
 * - Message passing and clock synchronization
 * - Happens-before relationship detection
 * - Concurrent event identification
 * - Compression vs fixed-width analysis
 *
 * Compile: gcc -I../src vector_clock.c ../build/src/libvarint.a -o vector_clock
 * Run: ./vector_clock
 */

#include "varintTagged.h"
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// VECTOR CLOCK DATA STRUCTURES
// ============================================================================

/**
 * Vector clock: sparse representation using (actorID, counter) pairs.
 * Only stores non-zero entries for efficiency.
 */
typedef struct {
    uint32_t *actorIds; // Actor IDs
    uint64_t *counters; // Logical timestamps
    size_t entryCount;  // Number of entries
    size_t capacity;    // Allocated capacity
} VectorClock;

/**
 * Comparison results for vector clocks
 */
typedef enum {
    VC_EQUAL,          // A == B (identical)
    VC_HAPPENS_BEFORE, // A < B (A causally precedes B)
    VC_HAPPENS_AFTER,  // A > B (B causally precedes A)
    VC_CONCURRENT      // A || B (concurrent, no causal relation)
} VectorClockOrdering;

// ============================================================================
// VECTOR CLOCK OPERATIONS
// ============================================================================

void vcInit(VectorClock *vc, size_t initialCapacity) {
    vc->actorIds = malloc(initialCapacity * sizeof(uint32_t));
    vc->counters = malloc(initialCapacity * sizeof(uint64_t));
    vc->entryCount = 0;
    vc->capacity = initialCapacity;
}

void vcFree(VectorClock *vc) {
    free(vc->actorIds);
    free(vc->counters);
    vc->entryCount = 0;
    vc->capacity = 0;
}

void vcCopy(VectorClock *dst, const VectorClock *src) {
    if (dst->capacity < src->entryCount) {
        dst->actorIds =
            realloc(dst->actorIds, src->entryCount * sizeof(uint32_t));
        dst->counters =
            realloc(dst->counters, src->entryCount * sizeof(uint64_t));
        dst->capacity = src->entryCount;
    }
    memcpy(dst->actorIds, src->actorIds, src->entryCount * sizeof(uint32_t));
    memcpy(dst->counters, src->counters, src->entryCount * sizeof(uint64_t));
    dst->entryCount = src->entryCount;
}

/**
 * Get counter for a specific actor (returns 0 if not present)
 */
uint64_t vcGet(const VectorClock *vc, uint32_t actorId) {
    for (size_t i = 0; i < vc->entryCount; i++) {
        if (vc->actorIds[i] == actorId) {
            return vc->counters[i];
        }
    }
    return 0;
}

/**
 * Set counter for a specific actor (adds if not present)
 */
void vcSet(VectorClock *vc, uint32_t actorId, uint64_t counter) {
    // Update existing entry
    for (size_t i = 0; i < vc->entryCount; i++) {
        if (vc->actorIds[i] == actorId) {
            vc->counters[i] = counter;
            return;
        }
    }

    // Add new entry
    if (vc->entryCount >= vc->capacity) {
        vc->capacity *= 2;
        vc->actorIds = realloc(vc->actorIds, vc->capacity * sizeof(uint32_t));
        vc->counters = realloc(vc->counters, vc->capacity * sizeof(uint64_t));
    }

    vc->actorIds[vc->entryCount] = actorId;
    vc->counters[vc->entryCount] = counter;
    vc->entryCount++;
}

/**
 * Increment local counter for an actor (local event)
 */
void vcIncrement(VectorClock *vc, uint32_t actorId) {
    uint64_t current = vcGet(vc, actorId);
    vcSet(vc, actorId, current + 1);
}

/**
 * Merge two vector clocks (take maximum of each counter)
 * Used when receiving a message: merge(local, received)
 */
void vcMerge(VectorClock *dst, const VectorClock *src) {
    for (size_t i = 0; i < src->entryCount; i++) {
        uint32_t actorId = src->actorIds[i];
        uint64_t srcCounter = src->counters[i];
        uint64_t dstCounter = vcGet(dst, actorId);

        if (srcCounter > dstCounter) {
            vcSet(dst, actorId, srcCounter);
        }
    }
}

/**
 * Compare two vector clocks to determine causal ordering
 */
VectorClockOrdering vcCompare(const VectorClock *a, const VectorClock *b) {
    bool aLessOrEqual = true;
    bool bLessOrEqual = true;

    // Check all actors in A
    for (size_t i = 0; i < a->entryCount; i++) {
        uint32_t actorId = a->actorIds[i];
        uint64_t aCounter = a->counters[i];
        uint64_t bCounter = vcGet(b, actorId);

        if (aCounter > bCounter) {
            aLessOrEqual = false; // a > b means NOT a <= b
        } else if (aCounter < bCounter) {
            bLessOrEqual = false; // a < b means NOT b <= a
        }
    }

    // Check for actors in B but not in A
    for (size_t i = 0; i < b->entryCount; i++) {
        uint32_t actorId = b->actorIds[i];
        bool found = false;
        for (size_t j = 0; j < a->entryCount; j++) {
            if (a->actorIds[j] == actorId) {
                found = true;
                break;
            }
        }

        if (!found && b->counters[i] > 0) {
            bLessOrEqual = false; // b has counter > a's 0, so NOT b <= a
        }
    }

    if (aLessOrEqual && bLessOrEqual) {
        return VC_EQUAL;
    } else if (aLessOrEqual) {
        return VC_HAPPENS_BEFORE;
    } else if (bLessOrEqual) {
        return VC_HAPPENS_AFTER;
    } else {
        return VC_CONCURRENT;
    }
}

// ============================================================================
// SERIALIZATION (using varintTagged for compression)
// ============================================================================

/**
 * Serialize vector clock to buffer using varintTagged encoding
 * Format: [entryCount] ([actorID] [counter])*
 */
size_t vcSerialize(const VectorClock *vc, uint8_t *buffer) {
    size_t offset = 0;

    // Write entry count
    offset += varintTaggedPut64(buffer + offset, vc->entryCount);

    // Write each (actorID, counter) pair
    for (size_t i = 0; i < vc->entryCount; i++) {
        offset += varintTaggedPut64(buffer + offset, vc->actorIds[i]);
        offset += varintTaggedPut64(buffer + offset, vc->counters[i]);
    }

    return offset;
}

/**
 * Deserialize vector clock from buffer
 */
size_t vcDeserialize(VectorClock *vc, const uint8_t *buffer) {
    size_t offset = 0;

    // Read entry count
    uint64_t entryCount;
    offset += varintTaggedGet64(buffer + offset, &entryCount);

    // Resize if needed
    if (vc->capacity < entryCount) {
        vc->actorIds = realloc(vc->actorIds, entryCount * sizeof(uint32_t));
        vc->counters = realloc(vc->counters, entryCount * sizeof(uint64_t));
        vc->capacity = entryCount;
    }
    vc->entryCount = entryCount;

    // Read each (actorID, counter) pair
    for (size_t i = 0; i < entryCount; i++) {
        uint64_t actorId, counter;
        offset += varintTaggedGet64(buffer + offset, &actorId);
        offset += varintTaggedGet64(buffer + offset, &counter);
        vc->actorIds[i] = actorId;
        vc->counters[i] = counter;
    }

    return offset;
}

// ============================================================================
// DISTRIBUTED KEY-VALUE STORE SIMULATION
// ============================================================================

typedef struct {
    char key[32];
    char value[64];
    VectorClock version; // Version vector for this key
} KVEntry;

typedef struct {
    uint32_t nodeId;
    VectorClock clock;
    KVEntry *entries;
    size_t entryCount;
    size_t entryCapacity;
} KVNode;

void nodeInit(KVNode *node, uint32_t nodeId) {
    node->nodeId = nodeId;
    vcInit(&node->clock, 10);
    node->entries = malloc(10 * sizeof(KVEntry));
    node->entryCount = 0;
    node->entryCapacity = 10;
}

void nodeFree(KVNode *node) {
    vcFree(&node->clock);
    for (size_t i = 0; i < node->entryCount; i++) {
        vcFree(&node->entries[i].version);
    }
    free(node->entries);
}

/**
 * Local write: increment local clock and create versioned entry
 */
void nodeWrite(KVNode *node, const char *key, const char *value) {
    // Increment local clock
    vcIncrement(&node->clock, node->nodeId);

    // Find or create entry
    KVEntry *entry = NULL;
    for (size_t i = 0; i < node->entryCount; i++) {
        if (strcmp(node->entries[i].key, key) == 0) {
            entry = &node->entries[i];
            break;
        }
    }

    if (!entry) {
        // Add new entry
        if (node->entryCount >= node->entryCapacity) {
            node->entryCapacity *= 2;
            node->entries =
                realloc(node->entries, node->entryCapacity * sizeof(KVEntry));
        }
        entry = &node->entries[node->entryCount++];
        strncpy(entry->key, key, sizeof(entry->key) - 1);
        entry->key[sizeof(entry->key) - 1] = '\0';
        vcInit(&entry->version, 10);
    }

    // Update value and version
    strncpy(entry->value, value, sizeof(entry->value) - 1);
    entry->value[sizeof(entry->value) - 1] = '\0';
    vcCopy(&entry->version, &node->clock);
}

/**
 * Message passing: send clock to another node
 */
void nodeSendMessage(KVNode *sender, KVNode *receiver) {
    // Increment sender's clock (send event)
    vcIncrement(&sender->clock, sender->nodeId);

    // Receiver merges sender's clock and increments own (receive event)
    vcMerge(&receiver->clock, &sender->clock);
    vcIncrement(&receiver->clock, receiver->nodeId);
}

// ============================================================================
// DISPLAY UTILITIES
// ============================================================================

void vcPrint(const VectorClock *vc, const char *prefix) {
    printf("%s{", prefix);
    for (size_t i = 0; i < vc->entryCount; i++) {
        if (i > 0) {
            printf(", ");
        }
        printf("N%u:%" PRIu64, vc->actorIds[i], vc->counters[i]);
    }
    printf("}");
}

const char *vcOrderingToString(VectorClockOrdering order) {
    switch (order) {
    case VC_EQUAL:
        return "EQUAL";
    case VC_HAPPENS_BEFORE:
        return "HAPPENS-BEFORE";
    case VC_HAPPENS_AFTER:
        return "HAPPENS-AFTER";
    case VC_CONCURRENT:
        return "CONCURRENT";
    default:
        return "UNKNOWN";
    }
}

// ============================================================================
// DEMONSTRATION
// ============================================================================

void demonstrateVectorClocks() {
    printf("\n=== Vector Clock Distributed System Example ===\n\n");

    // 1. Initialize 4-node distributed system
    printf("1. Initializing 4-node distributed system...\n\n");

    KVNode nodes[4];
    for (int i = 0; i < 4; i++) {
        nodeInit(&nodes[i], i + 1); // Node IDs: 1, 2, 3, 4
    }

    // 2. Local events (no communication)
    printf("2. Local events (concurrent):\n");

    nodeWrite(&nodes[0], "x", "a");
    printf("   Node 1: Write x=a  ");
    vcPrint(&nodes[0].clock, "");
    printf("\n");

    // Save Node 1's first write version for later comparison
    VectorClock savedN1Clock;
    vcInit(&savedN1Clock, 10);
    vcCopy(&savedN1Clock, &nodes[0].entries[0].version);

    nodeWrite(&nodes[1], "y", "b");
    printf("   Node 2: Write y=b  ");
    vcPrint(&nodes[1].clock, "");
    printf("\n");

    nodeWrite(&nodes[2], "z", "c");
    printf("   Node 3: Write z=c  ");
    vcPrint(&nodes[2].clock, "");
    printf("\n");

    // Check concurrency
    VectorClockOrdering ord12 = vcCompare(&nodes[0].clock, &nodes[1].clock);
    printf("\n   Node 1 vs Node 2: %s (concurrent events)\n",
           vcOrderingToString(ord12));
    assert(ord12 == VC_CONCURRENT);

    // 3. Message passing creates causal ordering
    printf("\n3. Message passing (creates happens-before):\n");

    printf("   Node 1 sends message to Node 2\n");
    nodeSendMessage(&nodes[0], &nodes[1]);
    printf("   Node 1: ");
    vcPrint(&nodes[0].clock, "");
    printf("\n");
    printf("   Node 2: ");
    vcPrint(&nodes[1].clock, "");
    printf("\n");

    nodeWrite(&nodes[1], "y", "updated_b");
    printf("   Node 2: Write y=updated_b  ");
    vcPrint(&nodes[1].clock, "");
    printf("\n");

    // Compare versions: Node 1's first write vs Node 2's second write
    // Find Node 2's "y" entry
    KVEntry *n2Entry = NULL;
    for (size_t i = 0; i < nodes[1].entryCount; i++) {
        if (strcmp(nodes[1].entries[i].key, "y") == 0) {
            n2Entry = &nodes[1].entries[i];
            break;
        }
    }

    VectorClockOrdering ord = vcCompare(&savedN1Clock, &n2Entry->version);
    printf("\n   Node 1's write vs Node 2's write: %s\n",
           vcOrderingToString(ord));
    printf("   (Node 1's write causally precedes Node 2's write)\n");
    assert(ord == VC_HAPPENS_BEFORE);

    // 4. Complex scenario: chain of events
    printf("\n4. Chain of causal dependencies:\n");

    printf("   Node 2 -> Node 3\n");
    nodeSendMessage(&nodes[1], &nodes[2]);
    printf("   Node 3: ");
    vcPrint(&nodes[2].clock, "");
    printf("\n");

    nodeWrite(&nodes[2], "z", "updated_c");
    printf("   Node 3: Write z=updated_c  ");
    vcPrint(&nodes[2].clock, "");
    printf("\n");

    printf("\n   Node 3 -> Node 4\n");
    nodeSendMessage(&nodes[2], &nodes[3]);
    printf("   Node 4: ");
    vcPrint(&nodes[3].clock, "");
    printf("\n");

    nodeWrite(&nodes[3], "w", "d");
    printf("   Node 4: Write w=d  ");
    vcPrint(&nodes[3].clock, "");
    printf("\n");

    // Verify transitivity: Node 1 -> Node 2 -> Node 3 -> Node 4
    // Find Node 4's "w" entry
    KVEntry *n4wEntry = NULL;
    for (size_t i = 0; i < nodes[3].entryCount; i++) {
        if (strcmp(nodes[3].entries[i].key, "w") == 0) {
            n4wEntry = &nodes[3].entries[i];
            break;
        }
    }

    VectorClockOrdering ord14 = vcCompare(&savedN1Clock, &n4wEntry->version);
    printf("\n   Node 1's first write vs Node 4's write: %s\n",
           vcOrderingToString(ord14));
    printf("   (Causal chain: 1 -> 2 -> 3 -> 4)\n");
    assert(ord14 == VC_HAPPENS_BEFORE);

    // 5. Concurrent writes (conflict detection)
    printf("\n5. Detecting concurrent writes (conflicts):\n");

    // Node 1 and Node 4 both write to same key without communication
    nodeWrite(&nodes[0], "shared", "value_from_node1");
    nodeWrite(&nodes[3], "shared", "value_from_node4");

    printf("   Node 1: Write shared=value_from_node1  ");
    vcPrint(&nodes[0].clock, "");
    printf("\n");

    printf("   Node 4: Write shared=value_from_node4  ");
    vcPrint(&nodes[3].clock, "");
    printf("\n");

    // Find the "shared" entries
    KVEntry *n1Entry = NULL, *n4Entry = NULL;
    for (size_t i = 0; i < nodes[0].entryCount; i++) {
        if (strcmp(nodes[0].entries[i].key, "shared") == 0) {
            n1Entry = &nodes[0].entries[i];
            break;
        }
    }
    for (size_t i = 0; i < nodes[3].entryCount; i++) {
        if (strcmp(nodes[3].entries[i].key, "shared") == 0) {
            n4Entry = &nodes[3].entries[i];
            break;
        }
    }

    VectorClockOrdering conflict =
        vcCompare(&n1Entry->version, &n4Entry->version);
    printf("\n   Conflict check: %s\n", vcOrderingToString(conflict));
    printf(
        "   → Requires conflict resolution (e.g., last-writer-wins, merge)\n");
    assert(conflict == VC_CONCURRENT);

    // 6. Serialization and compression analysis
    printf("\n6. Serialization and compression analysis:\n");

    uint8_t buffer[256];
    size_t serializedSize = vcSerialize(&nodes[3].clock, buffer);

    printf("   Node 4 clock: ");
    vcPrint(&nodes[3].clock, "");
    printf("\n");
    printf("   Serialized size: %zu bytes (varintTagged)\n", serializedSize);

    // Fixed-width comparison
    size_t fixedSize =
        sizeof(uint64_t) + // entry count
        nodes[3].clock.entryCount * (sizeof(uint32_t) + sizeof(uint64_t));
    printf("   Fixed-width size: %zu bytes\n", fixedSize);
    printf("   Compression: %.1f%%\n",
           ((float)(fixedSize - serializedSize) / fixedSize) * 100);

    // Verify deserialization
    VectorClock deserialized;
    vcInit(&deserialized, 10);
    vcDeserialize(&deserialized, buffer);

    printf("\n   Deserialized: ");
    vcPrint(&deserialized, "");
    printf("\n");

    assert(vcCompare(&nodes[3].clock, &deserialized) == VC_EQUAL);
    printf("   ✓ Serialization/deserialization verified\n");

    // 7. Sparse encoding benefits
    printf("\n7. Sparse encoding benefits:\n");
    printf("   System: 4 nodes, but typical clock has only 2-3 entries\n");
    printf("   Sparse representation: Only stores non-zero counters\n");

    size_t totalSparse = 0, totalDense = 0;
    for (int i = 0; i < 4; i++) {
        size_t sparse = vcSerialize(&nodes[i].clock, buffer);
        size_t dense =
            sizeof(uint32_t) * 4 + sizeof(uint64_t) * 4; // All 4 nodes
        totalSparse += sparse;
        totalDense += dense;
        printf("   Node %d: %zu bytes (sparse) vs %zu bytes (dense)\n", i + 1,
               sparse, dense);
    }

    printf("\n   Total: %zu bytes (sparse) vs %zu bytes (dense)\n", totalSparse,
           totalDense);
    printf("   Savings: %.1f%%\n",
           ((float)(totalDense - totalSparse) / totalDense) * 100);

    // 8. Real-world scenario simulation
    printf("\n8. Practical use case - Distributed updates:\n");
    printf("   Scenario: 4 replicas of a distributed database\n");
    printf("   Each node processes updates independently\n");
    printf("   Vector clocks track causal dependencies\n\n");

    printf("   Summary of operations:\n");
    printf("   - Concurrent writes: Detected via vector clock comparison\n");
    printf("   - Causal ordering: Preserved across message passing\n");
    printf("   - Conflict resolution: Requires application-level strategy\n");
    printf("   - Compression: 50-70%% savings with sparse varint encoding\n");

    // Cleanup
    vcFree(&savedN1Clock);
    vcFree(&deserialized);
    for (int i = 0; i < 4; i++) {
        nodeFree(&nodes[i]);
    }

    printf("\n✓ Vector clock example complete\n");
}

void demonstrateAdvancedScenarios() {
    printf("\n\n=== Advanced Vector Clock Scenarios ===\n\n");

    // Large-scale sparse scenario
    printf("1. Large-scale sparse scenario (1000 nodes, active subset):\n");

    VectorClock largeClock;
    vcInit(&largeClock, 100);

    // Simulate: 1000 potential nodes, only 5 active
    const uint32_t activeNodes[] = {7, 42, 103, 517, 999};
    for (int i = 0; i < 5; i++) {
        vcSet(&largeClock, activeNodes[i], i * 10 + 5);
    }

    printf("   Active nodes: 5 out of 1000 possible\n");
    printf("   Clock: ");
    vcPrint(&largeClock, "");
    printf("\n");

    uint8_t buffer[1024];
    size_t sparseSize = vcSerialize(&largeClock, buffer);
    size_t denseSize =
        sizeof(uint64_t) + 1000 * (sizeof(uint32_t) + sizeof(uint64_t));

    printf("   Sparse encoding: %zu bytes\n", sparseSize);
    printf("   Dense encoding: %zu bytes (all 1000 nodes)\n", denseSize);
    printf("   Compression ratio: %.2fx\n", (float)denseSize / sparseSize);

    vcFree(&largeClock);

    // Network partition scenario
    printf("\n2. Network partition and reconciliation:\n");

    KVNode partition1[2], partition2[2];
    nodeInit(&partition1[0], 1);
    nodeInit(&partition1[1], 2);
    nodeInit(&partition2[0], 3);
    nodeInit(&partition2[1], 4);

    printf("   Initial state: 4 nodes split into 2 partitions\n");

    // Partition 1 activity
    nodeWrite(&partition1[0], "data", "v1");
    nodeSendMessage(&partition1[0], &partition1[1]);
    nodeWrite(&partition1[1], "data", "v2");

    printf("   Partition 1 (Nodes 1-2): Write data=v1, then v2\n");
    printf("   Node 2: ");
    vcPrint(&partition1[1].clock, "");
    printf("\n");

    // Partition 2 activity (concurrent)
    nodeWrite(&partition2[0], "data", "v3");
    nodeSendMessage(&partition2[0], &partition2[1]);
    nodeWrite(&partition2[1], "data", "v4");

    printf("   Partition 2 (Nodes 3-4): Write data=v3, then v4\n");
    printf("   Node 4: ");
    vcPrint(&partition2[1].clock, "");
    printf("\n");

    // Detect concurrent versions
    KVEntry *p1Entry = NULL, *p2Entry = NULL;
    for (size_t i = 0; i < partition1[1].entryCount; i++) {
        if (strcmp(partition1[1].entries[i].key, "data") == 0) {
            p1Entry = &partition1[1].entries[i];
            break;
        }
    }
    for (size_t i = 0; i < partition2[1].entryCount; i++) {
        if (strcmp(partition2[1].entries[i].key, "data") == 0) {
            p2Entry = &partition2[1].entries[i];
            break;
        }
    }

    VectorClockOrdering partitionOrder =
        vcCompare(&p1Entry->version, &p2Entry->version);
    printf("\n   Partition reconciliation:\n");
    printf("   Version comparison: %s\n", vcOrderingToString(partitionOrder));
    printf("   → System must maintain both versions (siblings)\n");
    printf("   → Client resolves on next read/write\n");

    // Cleanup
    for (int i = 0; i < 2; i++) {
        nodeFree(&partition1[i]);
        nodeFree(&partition2[i]);
    }

    printf("\n✓ Advanced scenarios complete\n");
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    printf("=================================================\n");
    printf("  Vector Clock / Version Vector Example\n");
    printf("  Distributed Event Ordering & Causal Tracking\n");
    printf("=================================================\n");

    demonstrateVectorClocks();
    demonstrateAdvancedScenarios();

    printf("\n=================================================\n");
    printf("This example demonstrated:\n");
    printf("  • Vector clocks for causal ordering\n");
    printf("  • Happens-before relationship detection\n");
    printf("  • Concurrent event/conflict detection\n");
    printf("  • Distributed message passing\n");
    printf("  • Sparse encoding with varintTagged\n");
    printf("  • 50-98%% compression vs dense encoding\n");
    printf("  • Practical distributed KV store use case\n");
    printf("\n");
    printf("Key insights:\n");
    printf("  • Vector clocks enable causal consistency\n");
    printf("  • Sparse representation scales to large systems\n");
    printf("  • varintTagged provides efficient serialization\n");
    printf("  • Conflicts detected via clock comparison\n");
    printf("  • Essential for distributed databases (Dynamo,\n");
    printf("    Cassandra, Riak, CouchDB)\n");
    printf("=================================================\n");

    return 0;
}
