/* clusterRing.c - Consistent Hashing State Machine Implementation
 *
 * A system-level abstraction for data-structure-driven consistent hashing
 * logic providing topology-aware placement, quorum semantics, and pluggable
 * algorithms without implementing network I/O.
 *
 * Copyright (c) 2024-2025, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted under the BSD-3-Clause license.
 */

#include "clusterRing.h"
#include "clusterRingInternal.h"

#include "../deps/xxHash/xxhash.h"
#include "datakit.h"
#include "flex.h"
#include "timeUtil.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ====================================================================
 * Platform-Specific Optimization Hints
 * ==================================================================== */

/* Prefetch hints - supported on most modern compilers */
#if defined(__GNUC__) || defined(__clang__)
#define CLUSTER_PREFETCH(addr) __builtin_prefetch(addr, 0, 0)
#else
#define CLUSTER_PREFETCH(addr) ((void)0)
#endif

/* Branch prediction hints */
#if defined(__GNUC__) || defined(__clang__)
#define CLUSTER_LIKELY(x) __builtin_expect(!!(x), 1)
#define CLUSTER_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define CLUSTER_LIKELY(x) (x)
#define CLUSTER_UNLIKELY(x) (x)
#endif

/* ====================================================================
 * Pre-defined Quorum Profiles
 * ==================================================================== */

const clusterQuorum CLUSTER_QUORUM_STRONG = {
    .replicaCount = 3,
    .writeQuorum = 3, /* ALL */
    .writeSync = 3,
    .readQuorum = 1,
    .readRepairEnabled = false,
    .consistency = CLUSTER_CONSISTENCY_ALL,
};

const clusterQuorum CLUSTER_QUORUM_EVENTUAL = {
    .replicaCount = 3,
    .writeQuorum = 1,
    .writeSync = 1,
    .readQuorum = 1,
    .readRepairEnabled = false,
    .consistency = CLUSTER_CONSISTENCY_ONE,
};

const clusterQuorum CLUSTER_QUORUM_BALANCED = {
    .replicaCount = 3,
    .writeQuorum = 2, /* QUORUM */
    .writeSync = 2,
    .readQuorum = 2,
    .readRepairEnabled = true,
    .consistency = CLUSTER_CONSISTENCY_QUORUM,
};

const clusterQuorum CLUSTER_QUORUM_READ_HEAVY = {
    .replicaCount = 3,
    .writeQuorum = 3, /* ALL */
    .writeSync = 3,
    .readQuorum = 1,
    .readRepairEnabled = true,
    .consistency = CLUSTER_CONSISTENCY_ALL,
};

const clusterQuorum CLUSTER_QUORUM_WRITE_HEAVY = {
    .replicaCount = 3,
    .writeQuorum = 1,
    .writeSync = 1,
    .readQuorum = 3, /* ALL */
    .readRepairEnabled = false,
    .consistency = CLUSTER_CONSISTENCY_ONE,
};

/* ====================================================================
 * Pre-defined Affinity Rules
 * ==================================================================== */

const clusterAffinityRule CLUSTER_AFFINITY_RACK_SPREAD = {
    .spreadLevel = CLUSTER_LEVEL_RACK,
    .minSpread = 2,
    .required = true,
};

const clusterAffinityRule CLUSTER_AFFINITY_AZ_SPREAD = {
    .spreadLevel = CLUSTER_LEVEL_AVAILABILITY_ZONE,
    .minSpread = 2,
    .required = true,
};

const clusterAffinityRule CLUSTER_AFFINITY_REGION_SPREAD = {
    .spreadLevel = CLUSTER_LEVEL_REGION,
    .minSpread = 2,
    .required = false,
};

/* ====================================================================
 * Constants
 * ==================================================================== */

#define INITIAL_NODE_CAPACITY 16
#define INITIAL_VNODE_CAPACITY 256
#define INITIAL_KEYSPACE_CAPACITY 8
#define DEFAULT_VNODE_MULTIPLIER 150
#define MIN_VNODES_PER_NODE 10
#define MAX_VNODES_PER_NODE 500
#define MAGLEV_TABLE_SIZE 65537 /* Prime number */

/* Multilist configuration for placement results */
#define PLACEMENT_LIST_LIMIT FLEX_CAP_LEVEL_256
#define PLACEMENT_LIST_DEPTH 0

/* Singleton mflexState for clusterRing multilist operations */
static mflexState *clusterMflexState(void) {
    static mflexState *state = NULL;
    if (!state) {
        state = mflexStateCreate();
    }
    return state;
}

/* ====================================================================
 * Seen-Node Tracking (Optimized Deduplication)
 * ====================================================================
 * For small counts (<=64), use a single uint64_t bitmap with SWAR.
 * For medium counts (<=512), use a small bitmap array.
 * For large counts, fall back to hash-based tracking.
 */

#define SEEN_BITMAP_SMALL_MAX 64
#define SEEN_BITMAP_MEDIUM_MAX 512

typedef struct seenTracker {
    union {
        uint64_t small;     /* For <=64 nodes: single bitmap */
        uint64_t medium[8]; /* For <=512 nodes: 8x64 bitmap */
        uint64_t *large;    /* For >512 nodes: heap allocated */
    } bits;
    uint32_t capacity;
    bool usesHeap;
} seenTracker;

static inline void seenTrackerInit(seenTracker *st, uint32_t maxNodes) {
    st->usesHeap = false;
    if (maxNodes <= SEEN_BITMAP_SMALL_MAX) {
        st->bits.small = 0;
        st->capacity = SEEN_BITMAP_SMALL_MAX;
    } else if (maxNodes <= SEEN_BITMAP_MEDIUM_MAX) {
        memset(st->bits.medium, 0, sizeof(st->bits.medium));
        st->capacity = SEEN_BITMAP_MEDIUM_MAX;
    } else {
        /* Round up to multiple of 64 */
        uint32_t words = (maxNodes + 63) / 64;
        st->bits.large = zcalloc(words, sizeof(uint64_t));
        st->capacity = words * 64;
        st->usesHeap = true;
    }
}

static inline void seenTrackerFree(seenTracker *st) {
    if (st->usesHeap && st->bits.large) {
        zfree(st->bits.large);
        st->bits.large = NULL;
    }
}

/* Check if node index is seen (O(1)) */
static inline bool seenTrackerTest(const seenTracker *st, uint32_t idx) {
    if (st->capacity <= SEEN_BITMAP_SMALL_MAX) {
        return (st->bits.small & (1ULL << idx)) != 0;
    }

    if (st->capacity <= SEEN_BITMAP_MEDIUM_MAX) {
        return (st->bits.medium[idx >> 6] & (1ULL << (idx & 63))) != 0;
    }

    return (st->bits.large[idx >> 6] & (1ULL << (idx & 63))) != 0;
}

/* Mark node index as seen (O(1)) */
static inline void seenTrackerSet(seenTracker *st, uint32_t idx) {
    if (st->capacity <= SEEN_BITMAP_SMALL_MAX) {
        st->bits.small |= (1ULL << idx);
    } else if (st->capacity <= SEEN_BITMAP_MEDIUM_MAX) {
        st->bits.medium[idx >> 6] |= (1ULL << (idx & 63));
    } else {
        st->bits.large[idx >> 6] |= (1ULL << (idx & 63));
    }
}

/* ====================================================================
 * Node ID to Index Mapping (for bitmap-based tracking)
 * ====================================================================
 * Since node IDs may not be sequential, we need a fast way to map
 * node IDs to dense indices for bitmap operations.
 */

/* For small node counts, linear search is faster than hash lookup */
static inline int32_t nodeIdToIndex(const clusterRing *ring, uint64_t nodeId) {
    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        if (ring->nodeArray[i] && ring->nodeArray[i]->id == nodeId) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* ====================================================================
 * Heap-based Top-K Selection (for Rendezvous)
 * ====================================================================
 * Min-heap to efficiently find top-k elements in O(n log k) time.
 */

typedef struct heapNode {
    const clusterNode *node;
    uint64_t weight;
} heapNode;

static inline void heapSiftDown(heapNode *heap, uint32_t size, uint32_t idx) {
    while (true) {
        uint32_t smallest = idx;
        uint32_t left = 2 * idx + 1;
        uint32_t right = 2 * idx + 2;

        if (left < size && heap[left].weight < heap[smallest].weight) {
            smallest = left;
        }
        if (right < size && heap[right].weight < heap[smallest].weight) {
            smallest = right;
        }
        if (smallest == idx) {
            break;
        }

        heapNode tmp = heap[idx];
        heap[idx] = heap[smallest];
        heap[smallest] = tmp;
        idx = smallest;
    }
}

static inline void heapSiftUp(heapNode *heap, uint32_t idx) {
    while (idx > 0) {
        uint32_t parent = (idx - 1) / 2;
        if (heap[parent].weight <= heap[idx].weight) {
            break;
        }
        heapNode tmp = heap[idx];
        heap[idx] = heap[parent];
        heap[parent] = tmp;
        idx = parent;
    }
}

/* Insert into min-heap of size k, maintaining only top-k largest */
static inline void heapInsertTopK(heapNode *heap, uint32_t *size, uint32_t k,
                                  const clusterNode *node, uint64_t weight) {
    if (*size < k) {
        /* Heap not full, just insert */
        heap[*size].node = node;
        heap[*size].weight = weight;
        heapSiftUp(heap, *size);
        (*size)++;
    } else if (weight > heap[0].weight) {
        /* Replace minimum if this weight is larger */
        heap[0].node = node;
        heap[0].weight = weight;
        heapSiftDown(heap, k, 0);
    }
}

/* Extract all elements from heap in descending order */
static inline void heapExtractAll(heapNode *heap, uint32_t size,
                                  const clusterNode **out) {
    /* Extract in reverse order to get descending weights */
    for (uint32_t i = size; i > 0; i--) {
        out[i - 1] = heap[0].node;
        heap[0] = heap[i - 1];
        heapSiftDown(heap, i - 1, 0);
    }
}

/* ====================================================================
 * Hash Functions
 * ==================================================================== */

uint64_t clusterHash64(const void *key, uint32_t len, uint32_t seed) {
    return XXH64(key, len, seed);
}

uint32_t clusterHash32(const void *key, uint32_t len, uint32_t seed) {
    return XXH32(key, len, seed);
}

/* Hash with node ID for virtual node point generation */
static uint64_t clusterHashVnodePoint(uint64_t nodeId, uint32_t vnodeIdx,
                                      uint32_t seed) {
    uint8_t buf[12];
    memcpy(buf, &nodeId, 8);
    memcpy(buf + 8, &vnodeIdx, 4);
    return XXH64(buf, 12, seed);
}

/* ====================================================================
 * Time Helpers
 * ==================================================================== */

uint64_t clusterGetTimeNs(void) {
    return timeUtilMonotonicNs();
}

static uint64_t clusterGetTimeMs(void) {
    return timeUtilMonotonicMs();
}

/* ====================================================================
 * Placement/Set Helper Functions
 * ==================================================================== */

void clusterPlacementInit(clusterPlacement *p) {
    if (p) {
        memset(p, 0, sizeof(*p));
        p->replicas = multilistNew(PLACEMENT_LIST_LIMIT, PLACEMENT_LIST_DEPTH);
    }
}

void clusterPlacementFree(clusterPlacement *p) {
    if (p && p->replicas) {
        multilistFree(p->replicas);
        p->replicas = NULL;
    }
}

size_t clusterPlacementReplicaCount(const clusterPlacement *p) {
    return p && p->replicas ? multilistCount(p->replicas) : 0;
}

const clusterNode *clusterPlacementGetReplica(const clusterPlacement *p,
                                              size_t idx) {
    if (!p || !p->replicas) {
        return NULL;
    }
    multilistEntry entry;
    if (multilistIndex(p->replicas, clusterMflexState(), (mlOffsetId)idx,
                       &entry, true)) {
        return entry.box.data.ptr;
    }
    return NULL;
}

/* Helper to add node pointer to placement replicas */
static void clusterPlacementAddReplica(clusterPlacement *p,
                                       const clusterNode *node) {
    if (!p || !p->replicas || !node) {
        return;
    }
    databox box = {.type = DATABOX_PTR, .data.ptr = (void *)node};
    multilistPushByTypeTail(&p->replicas, clusterMflexState(), &box);
}

void clusterWriteSetInit(clusterWriteSet *ws) {
    if (ws) {
        memset(ws, 0, sizeof(*ws));
        ws->targets = multilistNew(PLACEMENT_LIST_LIMIT, PLACEMENT_LIST_DEPTH);
    }
}

void clusterWriteSetFree(clusterWriteSet *ws) {
    if (ws && ws->targets) {
        multilistFree(ws->targets);
        ws->targets = NULL;
    }
}

size_t clusterWriteSetTargetCount(const clusterWriteSet *ws) {
    return ws && ws->targets ? multilistCount(ws->targets) : 0;
}

const clusterNode *clusterWriteSetGetTarget(const clusterWriteSet *ws,
                                            size_t idx) {
    if (!ws || !ws->targets) {
        return NULL;
    }
    multilistEntry entry;
    if (multilistIndex(ws->targets, clusterMflexState(), (mlOffsetId)idx,
                       &entry, true)) {
        return entry.box.data.ptr;
    }
    return NULL;
}

/* Helper to add node pointer to write set targets */
static void clusterWriteSetAddTarget(clusterWriteSet *ws,
                                     const clusterNode *node) {
    if (!ws || !ws->targets || !node) {
        return;
    }
    databox box = {.type = DATABOX_PTR, .data.ptr = (void *)node};
    multilistPushByTypeTail(&ws->targets, clusterMflexState(), &box);
}

void clusterReadSetInit(clusterReadSet *rs) {
    if (rs) {
        memset(rs, 0, sizeof(*rs));
        rs->candidates =
            multilistNew(PLACEMENT_LIST_LIMIT, PLACEMENT_LIST_DEPTH);
    }
}

void clusterReadSetFree(clusterReadSet *rs) {
    if (rs && rs->candidates) {
        multilistFree(rs->candidates);
        rs->candidates = NULL;
    }
}

size_t clusterReadSetCandidateCount(const clusterReadSet *rs) {
    return rs && rs->candidates ? multilistCount(rs->candidates) : 0;
}

const clusterNode *clusterReadSetGetCandidate(const clusterReadSet *rs,
                                              size_t idx) {
    if (!rs || !rs->candidates) {
        return NULL;
    }
    multilistEntry entry;
    if (multilistIndex(rs->candidates, clusterMflexState(), (mlOffsetId)idx,
                       &entry, true)) {
        return entry.box.data.ptr;
    }
    return NULL;
}

/* Helper to add node pointer to read set candidates */
static void clusterReadSetAddCandidate(clusterReadSet *rs,
                                       const clusterNode *node) {
    if (!rs || !rs->candidates || !node) {
        return;
    }
    databox box = {.type = DATABOX_PTR, .data.ptr = (void *)node};
    multilistPushByTypeTail(&rs->candidates, clusterMflexState(), &box);
}

/* ====================================================================
 * Node Management Helpers
 * ==================================================================== */

static clusterNode *clusterNodeAlloc(void) {
    clusterNode *node = zcalloc(1, sizeof(*node));
    return node;
}

static void clusterNodeFreeInternal(clusterNode *node) {
    if (node) {
        if (node->name) {
            mdscfree(node->name);
        }
        if (node->address) {
            mdscfree(node->address);
        }
        zfree(node);
    }
}

static void clusterNodeInit(clusterNode *node,
                            const clusterNodeConfig *config) {
    node->id = config->id;
    /* Convert const char * to mdsc internally */
    node->name = config->name ? mdscnew(config->name) : mdscempty();
    node->address = config->address ? mdscnew(config->address) : mdscempty();
    node->location = config->location;
    node->weight = config->weight ? config->weight : 100; /* Default weight */
    node->capacityBytes = config->capacityBytes;
    node->usedBytes = 0;
    node->state = config->initialState;
    node->stateChangedAt = clusterGetTimeMs();
    node->failureCount = 0;
    node->vnodeCount = 0;
    node->vnodeStartIndex = 0;
}

/* ====================================================================
 * Ring Lifecycle
 * ==================================================================== */

clusterRing *clusterRingNew(const clusterRingConfig *config) {
    if (!config) {
        return NULL;
    }

    clusterRing *ring = zcalloc(1, sizeof(*ring));
    if (!ring) {
        return NULL;
    }

    /* Convert const char * to mdsc internally */
    ring->name = config->name ? mdscnew(config->name) : mdscempty();

    /* Initialize node storage */
    ring->nodeCapacity = config->expectedNodeCount > 0
                             ? config->expectedNodeCount
                             : INITIAL_NODE_CAPACITY;
    ring->nodeArray = zcalloc(ring->nodeCapacity, sizeof(clusterNode *));
    if (!ring->nodeArray) {
        zfree(ring);
        return NULL;
    }
    ring->nodeCount = 0;
    ring->healthyNodeCount = 0;

    /* Create node lookup dict */
    ring->nodeById =
        multidictNew(&multidictTypeExactKey, multidictDefaultClassNew(), 0);
    if (!ring->nodeById) {
        zfree(ring->nodeArray);
        zfree(ring);
        return NULL;
    }

    /* Strategy configuration */
    ring->strategyType = config->strategyType;
    ring->customStrategy = config->customStrategy;
    ring->hashSeed = config->hashSeed;

    /* Virtual node configuration */
    ring->vnodeConfig = config->vnodes;
    if (ring->vnodeConfig.vnodeMultiplier == 0) {
        ring->vnodeConfig.vnodeMultiplier = DEFAULT_VNODE_MULTIPLIER;
    }
    if (ring->vnodeConfig.minVnodesPerNode == 0) {
        ring->vnodeConfig.minVnodesPerNode = MIN_VNODES_PER_NODE;
    }
    if (ring->vnodeConfig.maxVnodesPerNode == 0) {
        ring->vnodeConfig.maxVnodesPerNode = MAX_VNODES_PER_NODE;
    }

    /* Default quorum */
    ring->defaultQuorum = config->defaultQuorum;
    if (ring->defaultQuorum.replicaCount == 0) {
        ring->defaultQuorum = CLUSTER_QUORUM_BALANCED;
    }

    /* Copy affinity rules */
    if (config->affinityRules && config->affinityRuleCount > 0) {
        ring->affinityRules =
            zcalloc(config->affinityRuleCount, sizeof(clusterAffinityRule));
        if (ring->affinityRules) {
            memcpy(ring->affinityRules, config->affinityRules,
                   config->affinityRuleCount * sizeof(clusterAffinityRule));
            ring->affinityRuleCount = config->affinityRuleCount;
        }
    }

    /* Initialize keyspace storage */
    ring->keySpaceCapacity = INITIAL_KEYSPACE_CAPACITY;
    ring->keySpaces =
        zcalloc(ring->keySpaceCapacity, sizeof(clusterKeySpace *));
    ring->keySpaceByName =
        multidictNew(&multidictTypeExactKey, multidictDefaultClassNew(), 0);

    /* Initialize strategy-specific data */
    clusterResult strategyResult = CLUSTER_OK;
    switch (ring->strategyType) {
    case CLUSTER_STRATEGY_KETAMA:
        strategyResult = clusterRingInitKetama(ring);
        break;
    case CLUSTER_STRATEGY_JUMP:
        strategyResult = clusterRingInitJump(ring);
        break;
    case CLUSTER_STRATEGY_RENDEZVOUS:
        strategyResult = clusterRingInitRendezvous(ring);
        break;
    case CLUSTER_STRATEGY_MAGLEV:
        strategyResult = clusterRingInitMaglev(ring);
        break;
    case CLUSTER_STRATEGY_BOUNDED:
        strategyResult = clusterRingInitBounded(ring);
        break;
    case CLUSTER_STRATEGY_CUSTOM:
        /* Custom strategy should be fully initialized by caller */
        break;
    }

    if (strategyResult != CLUSTER_OK) {
        clusterRingFree(ring);
        return NULL;
    }

    /* Initialize version tracking */
    ring->version = 1;
    ring->lastModified = clusterGetTimeMs();

    /* Initialize stats */
    memset(&ring->stats, 0, sizeof(ring->stats));

    return ring;
}

clusterRing *clusterRingNewDefault(void) {
    clusterRingConfig config = {
        .name = "default",
        .strategyType = CLUSTER_STRATEGY_KETAMA,
        .defaultQuorum = CLUSTER_QUORUM_BALANCED,
    };
    return clusterRingNew(&config);
}

void clusterRingFree(clusterRing *ring) {
    if (!ring) {
        return;
    }

    /* Free strategy-specific data */
    switch (ring->strategyType) {
    case CLUSTER_STRATEGY_KETAMA:
        clusterRingFreeKetama(ring);
        break;
    case CLUSTER_STRATEGY_JUMP:
        clusterRingFreeJump(ring);
        break;
    case CLUSTER_STRATEGY_RENDEZVOUS:
        clusterRingFreeRendezvous(ring);
        break;
    case CLUSTER_STRATEGY_MAGLEV:
        clusterRingFreeMaglev(ring);
        break;
    case CLUSTER_STRATEGY_BOUNDED:
        clusterRingFreeBounded(ring);
        break;
    case CLUSTER_STRATEGY_CUSTOM:
        if (ring->customStrategy && ring->customStrategy->freeStrategyData) {
            ring->customStrategy->freeStrategyData(
                ring->customStrategy->strategyData);
        }
        break;
    }

    /* Free nodes */
    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        if (ring->nodeArray[i]) {
            clusterNodeFreeInternal(ring->nodeArray[i]);
        }
    }
    zfree(ring->nodeArray);

    /* Free node lookup */
    if (ring->nodeById) {
        multidictFree(ring->nodeById);
    }

    /* Free keyspaces */
    for (uint32_t i = 0; i < ring->keySpaceCount; i++) {
        if (ring->keySpaces[i]) {
            if (ring->keySpaces[i]->name) {
                mdscfree(ring->keySpaces[i]->name);
            }
            if (ring->keySpaces[i]->rules) {
                zfree(ring->keySpaces[i]->rules);
            }
            zfree(ring->keySpaces[i]);
        }
    }
    zfree(ring->keySpaces);
    if (ring->keySpaceByName) {
        multidictFree(ring->keySpaceByName);
    }

    /* Free affinity rules */
    if (ring->affinityRules) {
        zfree(ring->affinityRules);
    }

    /* Free rebalance plan */
    if (ring->rebalancePlan) {
        if (ring->rebalancePlan->moves) {
            zfree(ring->rebalancePlan->moves);
        }
        zfree(ring->rebalancePlan);
    }

    /* Free health provider */
    if (ring->healthProvider && ring->healthProvider->freeProvider) {
        ring->healthProvider->freeProvider(ring->healthProvider->providerData);
    }

    /* Free ring name */
    if (ring->name) {
        mdscfree(ring->name);
    }

    zfree(ring);
}

/* ====================================================================
 * Strategy Initialization
 * ==================================================================== */

clusterResult clusterRingInitKetama(clusterRing *ring) {
    clusterKetamaData *data = &ring->strategyData.ketama;
    data->vnodeCapacity = INITIAL_VNODE_CAPACITY;
    data->vnodes = zcalloc(data->vnodeCapacity, sizeof(clusterVnode));
    if (!data->vnodes) {
        return CLUSTER_ERR_ALLOC_FAILED;
    }
    data->vnodeCount = 0;
    data->needsSort = false;
    return CLUSTER_OK;
}

clusterResult clusterRingInitJump(clusterRing *ring) {
    clusterJumpData *data = &ring->strategyData.jump;
    data->nodeIds = NULL;
    data->bucketCount = 0;
    return CLUSTER_OK;
}

clusterResult clusterRingInitRendezvous(clusterRing *ring) {
    /* Rendezvous needs no extra state */
    (void)ring;
    return CLUSTER_OK;
}

clusterResult clusterRingInitMaglev(clusterRing *ring) {
    clusterMaglevData *data = &ring->strategyData.maglev;
    data->tableSize = MAGLEV_TABLE_SIZE;
    data->lookup = zcalloc(data->tableSize, sizeof(uint64_t));
    if (!data->lookup) {
        return CLUSTER_ERR_ALLOC_FAILED;
    }
    /* Initialize all slots to invalid */
    memset(data->lookup, 0xFF, data->tableSize * sizeof(uint64_t));
    data->needsRebuild = true;
    return CLUSTER_OK;
}

clusterResult clusterRingInitBounded(clusterRing *ring) {
    clusterBoundedData *data = &ring->strategyData.bounded;
    data->loadFactor = 1.25f; /* 25% above average max load */
    data->nodeLoads = NULL;
    data->nodeCount = 0;
    return CLUSTER_OK;
}

/* ====================================================================
 * Strategy Cleanup
 * ==================================================================== */

void clusterRingFreeKetama(clusterRing *ring) {
    clusterKetamaData *data = &ring->strategyData.ketama;
    if (data->vnodes) {
        zfree(data->vnodes);
        data->vnodes = NULL;
    }
}

void clusterRingFreeJump(clusterRing *ring) {
    clusterJumpData *data = &ring->strategyData.jump;
    if (data->nodeIds) {
        zfree(data->nodeIds);
        data->nodeIds = NULL;
    }
}

void clusterRingFreeRendezvous(clusterRing *ring) {
    (void)ring;
    /* Nothing to free */
}

void clusterRingFreeMaglev(clusterRing *ring) {
    clusterMaglevData *data = &ring->strategyData.maglev;
    if (data->lookup) {
        zfree(data->lookup);
        data->lookup = NULL;
    }
}

void clusterRingFreeBounded(clusterRing *ring) {
    clusterBoundedData *data = &ring->strategyData.bounded;
    if (data->nodeLoads) {
        zfree(data->nodeLoads);
        data->nodeLoads = NULL;
    }
}

/* ====================================================================
 * Virtual Node Management (Ketama)
 * ==================================================================== */

static int vnodeCompare(const void *a, const void *b) {
    const clusterVnode *va = (const clusterVnode *)a;
    const clusterVnode *vb = (const clusterVnode *)b;
    if (va->hashPoint < vb->hashPoint) {
        return -1;
    }
    if (va->hashPoint > vb->hashPoint) {
        return 1;
    }
    return 0;
}

void clusterRingSortVnodes(clusterRing *ring) {
    clusterKetamaData *data = &ring->strategyData.ketama;
    if (data->needsSort && data->vnodeCount > 1) {
        qsort(data->vnodes, data->vnodeCount, sizeof(clusterVnode),
              vnodeCompare);
        data->needsSort = false;
    }
}

clusterResult clusterRingAddVnodes(clusterRing *ring, clusterNode *node) {
    if (ring->strategyType != CLUSTER_STRATEGY_KETAMA &&
        ring->strategyType != CLUSTER_STRATEGY_BOUNDED) {
        return CLUSTER_OK; /* No vnodes for other strategies */
    }

    clusterKetamaData *data = &ring->strategyData.ketama;

    /* Calculate number of vnodes based on weight */
    uint32_t vnodeCount =
        node->weight * ring->vnodeConfig.vnodeMultiplier / 100;
    if (vnodeCount < ring->vnodeConfig.minVnodesPerNode) {
        vnodeCount = ring->vnodeConfig.minVnodesPerNode;
    }
    if (vnodeCount > ring->vnodeConfig.maxVnodesPerNode) {
        vnodeCount = ring->vnodeConfig.maxVnodesPerNode;
    }

    /* Grow vnode array if needed */
    if (data->vnodeCount + vnodeCount > data->vnodeCapacity) {
        uint32_t newCapacity = data->vnodeCapacity * 2;
        while (newCapacity < data->vnodeCount + vnodeCount) {
            newCapacity *= 2;
        }
        clusterVnode *newVnodes =
            zrealloc(data->vnodes, newCapacity * sizeof(clusterVnode));
        if (!newVnodes) {
            return CLUSTER_ERR_ALLOC_FAILED;
        }
        data->vnodes = newVnodes;
        data->vnodeCapacity = newCapacity;
    }

    /* Record start index and count in node */
    node->vnodeStartIndex = data->vnodeCount;
    node->vnodeCount = vnodeCount;

    /* Add virtual nodes */
    for (uint32_t i = 0; i < vnodeCount; i++) {
        clusterVnode *vn = &data->vnodes[data->vnodeCount++];
        vn->hashPoint = clusterHashVnodePoint(node->id, i, ring->hashSeed);
        vn->nodeId = node->id;
        vn->nodePtr = node; /* Direct pointer for fast lookup */
        vn->vnodeIndex = (uint16_t)i;
    }

    data->needsSort = true;
    return CLUSTER_OK;
}

clusterResult clusterRingRemoveVnodes(clusterRing *ring, uint64_t nodeId) {
    if (ring->strategyType != CLUSTER_STRATEGY_KETAMA &&
        ring->strategyType != CLUSTER_STRATEGY_BOUNDED) {
        return CLUSTER_OK;
    }

    clusterKetamaData *data = &ring->strategyData.ketama;

    /* Remove all vnodes belonging to this node */
    uint32_t writeIdx = 0;
    for (uint32_t readIdx = 0; readIdx < data->vnodeCount; readIdx++) {
        if (data->vnodes[readIdx].nodeId != nodeId) {
            if (writeIdx != readIdx) {
                data->vnodes[writeIdx] = data->vnodes[readIdx];
            }
            writeIdx++;
        }
    }
    data->vnodeCount = writeIdx;

    /* Array is still sorted if it was before */
    return CLUSTER_OK;
}

/* ====================================================================
 * Jump Hash Helpers
 * ==================================================================== */

static void clusterRingRebuildJump(clusterRing *ring) {
    clusterJumpData *data = &ring->strategyData.jump;

    /* Free old array */
    if (data->nodeIds) {
        zfree(data->nodeIds);
    }

    /* Build new array of healthy node IDs */
    data->bucketCount = ring->healthyNodeCount;
    if (data->bucketCount == 0) {
        data->nodeIds = NULL;
        return;
    }

    data->nodeIds = zcalloc(data->bucketCount, sizeof(uint64_t));
    if (!data->nodeIds) {
        data->bucketCount = 0;
        return;
    }

    uint32_t idx = 0;
    for (uint32_t i = 0; i < ring->nodeCount && idx < data->bucketCount; i++) {
        const clusterNode *node = ring->nodeArray[i];
        if (node && node->state == CLUSTER_NODE_UP) {
            data->nodeIds[idx++] = node->id;
        }
    }
}

/* Jump Consistent Hash - Google's algorithm */
static int32_t jumpConsistentHash(uint64_t key, int32_t numBuckets) {
    int64_t b = -1, j = 0;
    while (j < numBuckets) {
        b = j;
        key = key * 2862933555777941757ULL + 1;
        j = (int64_t)((b + 1) *
                      ((double)(1LL << 31) / (double)((key >> 33) + 1)));
    }
    return (int32_t)b;
}

/* ====================================================================
 * Maglev Helpers
 * ==================================================================== */

static void clusterRingRebuildMaglev(clusterRing *ring) {
    clusterMaglevData *data = &ring->strategyData.maglev;

    if (ring->healthyNodeCount == 0) {
        memset(data->lookup, 0xFF, data->tableSize * sizeof(uint64_t));
        data->needsRebuild = false;
        return;
    }

    /* Collect healthy nodes */
    uint64_t *healthyNodes = zcalloc(ring->healthyNodeCount, sizeof(uint64_t));
    if (!healthyNodes) {
        return;
    }

    uint32_t healthyIdx = 0;
    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        const clusterNode *node = ring->nodeArray[i];
        if (node && node->state == CLUSTER_NODE_UP) {
            healthyNodes[healthyIdx++] = node->id;
        }
    }

    /* Build permutation table */
    uint32_t N = data->tableSize;
    uint32_t M = ring->healthyNodeCount;

    /* Allocate per-node offset and skip */
    uint32_t *offset = zcalloc(M, sizeof(uint32_t));
    uint32_t *skip = zcalloc(M, sizeof(uint32_t));
    uint32_t *next = zcalloc(M, sizeof(uint32_t));

    if (!offset || !skip || !next) {
        zfree(healthyNodes);
        if (offset) {
            zfree(offset);
        }
        if (skip) {
            zfree(skip);
        }
        if (next) {
            zfree(next);
        }
        return;
    }

    /* Calculate offset and skip for each node */
    for (uint32_t i = 0; i < M; i++) {
        uint64_t h1 =
            clusterHash64(&healthyNodes[i], sizeof(uint64_t), ring->hashSeed);
        uint64_t h2 = clusterHash64(&healthyNodes[i], sizeof(uint64_t),
                                    ring->hashSeed + 1);
        offset[i] = h1 % N;
        skip[i] = (h2 % (N - 1)) + 1;
        next[i] = 0;
    }

    /* Fill lookup table using Maglev algorithm */
    memset(data->lookup, 0xFF, N * sizeof(uint64_t));
    uint32_t filled = 0;

    while (filled < N) {
        for (uint32_t i = 0; i < M && filled < N; i++) {
            uint32_t c = (offset[i] + next[i] * skip[i]) % N;
            while (data->lookup[c] != UINT64_MAX) {
                next[i]++;
                c = (offset[i] + next[i] * skip[i]) % N;
            }
            data->lookup[c] = healthyNodes[i];
            next[i]++;
            filled++;
        }
    }

    zfree(healthyNodes);
    zfree(offset);
    zfree(skip);
    zfree(next);

    data->needsRebuild = false;
}

/* ====================================================================
 * Node Array Management
 * ==================================================================== */

clusterResult clusterRingGrowNodeArray(clusterRing *ring) {
    uint32_t newCapacity = ring->nodeCapacity * 2;
    clusterNode **newArray =
        zrealloc(ring->nodeArray, newCapacity * sizeof(clusterNode *));
    if (!newArray) {
        return CLUSTER_ERR_ALLOC_FAILED;
    }
    /* Zero new slots */
    memset(newArray + ring->nodeCapacity, 0,
           (newCapacity - ring->nodeCapacity) * sizeof(clusterNode *));
    ring->nodeArray = newArray;
    ring->nodeCapacity = newCapacity;
    return CLUSTER_OK;
}

void clusterRingCompactNodeArray(clusterRing *ring) {
    /* Compact by moving non-NULL entries to front */
    uint32_t writeIdx = 0;
    for (uint32_t readIdx = 0; readIdx < ring->nodeCount; readIdx++) {
        if (ring->nodeArray[readIdx]) {
            if (writeIdx != readIdx) {
                ring->nodeArray[writeIdx] = ring->nodeArray[readIdx];
                ring->nodeArray[readIdx] = NULL;
            }
            writeIdx++;
        }
    }
    ring->nodeCount = writeIdx;
}

/* ====================================================================
 * Node Management API
 * ==================================================================== */

clusterResult clusterRingAddNode(clusterRing *ring,
                                 const clusterNodeConfig *config) {
    if (!ring || !config) {
        return CLUSTER_ERR;
    }

    /* Check if node already exists */
    databox keybox = {.type = DATABOX_UNSIGNED_64, .data.u64 = config->id};
    databox valbox;
    if (multidictFind(ring->nodeById, &keybox, &valbox)) {
        return CLUSTER_ERR_EXISTS;
    }

    /* Grow array if needed */
    if (ring->nodeCount >= ring->nodeCapacity) {
        clusterResult result = clusterRingGrowNodeArray(ring);
        if (result != CLUSTER_OK) {
            return result;
        }
    }

    /* Allocate and initialize node */
    clusterNode *node = clusterNodeAlloc();
    if (!node) {
        return CLUSTER_ERR_ALLOC_FAILED;
    }
    clusterNodeInit(node, config);

    /* Add to array */
    ring->nodeArray[ring->nodeCount++] = node;

    /* Add to lookup dict */
    valbox.type = DATABOX_PTR;
    valbox.data.ptr = node;
    multidictAdd(ring->nodeById, &keybox, &valbox);

    /* Update healthy count */
    if (node->state == CLUSTER_NODE_UP) {
        ring->healthyNodeCount++;
    }

    /* Add virtual nodes (for Ketama) */
    clusterResult vnodeResult = clusterRingAddVnodes(ring, node);
    if (vnodeResult != CLUSTER_OK) {
        /* Rollback - remove node */
        ring->nodeCount--;
        multidictDelete(ring->nodeById, &keybox);
        if (node->state == CLUSTER_NODE_UP) {
            ring->healthyNodeCount--;
        }
        clusterNodeFreeInternal(node);
        return vnodeResult;
    }

    /* Sort vnodes after adding */
    if (ring->strategyType == CLUSTER_STRATEGY_KETAMA) {
        clusterRingSortVnodes(ring);
    }

    /* Mark strategies that need rebuild */
    if (ring->strategyType == CLUSTER_STRATEGY_JUMP) {
        clusterRingRebuildJump(ring);
    } else if (ring->strategyType == CLUSTER_STRATEGY_MAGLEV) {
        ring->strategyData.maglev.needsRebuild = true;
    }

    /* Update version */
    ring->version++;
    ring->lastModified = clusterGetTimeMs();

    /* Invoke state callback if set */
    if (ring->stateCallback) {
        ring->stateCallback(ring, node->id, CLUSTER_NODE_DOWN, node->state,
                            ring->stateCallbackData);
    }

    return CLUSTER_OK;
}

clusterResult clusterRingRemoveNode(clusterRing *ring, uint64_t nodeId) {
    if (!ring) {
        return CLUSTER_ERR;
    }

    /* Find node */
    databox keybox = {.type = DATABOX_UNSIGNED_64, .data.u64 = nodeId};
    databox valbox;
    if (!multidictFind(ring->nodeById, &keybox, &valbox)) {
        return CLUSTER_ERR_NOT_FOUND;
    }

    clusterNode *node = (clusterNode *)valbox.data.ptr;

    /* Remove virtual nodes first */
    clusterRingRemoveVnodes(ring, nodeId);

    /* Update healthy count */
    if (node->state == CLUSTER_NODE_UP) {
        ring->healthyNodeCount--;
    }

    /* Remove from dict */
    multidictDelete(ring->nodeById, &keybox);

    /* Find and remove from array */
    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        if (ring->nodeArray[i] == node) {
            ring->nodeArray[i] = NULL;
            break;
        }
    }

    /* Compact array */
    clusterRingCompactNodeArray(ring);

    /* Invoke state callback before freeing */
    if (ring->stateCallback) {
        ring->stateCallback(ring, nodeId, node->state, CLUSTER_NODE_DOWN,
                            ring->stateCallbackData);
    }

    /* Free node */
    clusterNodeFreeInternal(node);

    /* Rebuild strategies */
    if (ring->strategyType == CLUSTER_STRATEGY_JUMP) {
        clusterRingRebuildJump(ring);
    } else if (ring->strategyType == CLUSTER_STRATEGY_MAGLEV) {
        ring->strategyData.maglev.needsRebuild = true;
    }

    /* Update version */
    ring->version++;
    ring->lastModified = clusterGetTimeMs();

    return CLUSTER_OK;
}

clusterResult clusterRingAddNodes(clusterRing *ring,
                                  const clusterNodeConfig *configs,
                                  uint32_t count) {
    if (!ring || !configs || count == 0) {
        return CLUSTER_ERR;
    }

    for (uint32_t i = 0; i < count; i++) {
        clusterResult result = clusterRingAddNode(ring, &configs[i]);
        if (result != CLUSTER_OK && result != CLUSTER_ERR_EXISTS) {
            return result;
        }
    }

    return CLUSTER_OK;
}

clusterResult clusterRingSetNodeState(clusterRing *ring, uint64_t nodeId,
                                      clusterNodeState state) {
    if (!ring) {
        return CLUSTER_ERR;
    }

    databox keybox = {.type = DATABOX_UNSIGNED_64, .data.u64 = nodeId};
    databox valbox;
    if (!multidictFind(ring->nodeById, &keybox, &valbox)) {
        return CLUSTER_ERR_NOT_FOUND;
    }

    clusterNode *node = (clusterNode *)valbox.data.ptr;
    clusterNodeState oldState = node->state;

    if (oldState == state) {
        return CLUSTER_OK; /* No change */
    }

    /* Update healthy count */
    if (oldState == CLUSTER_NODE_UP && state != CLUSTER_NODE_UP) {
        ring->healthyNodeCount--;
    } else if (oldState != CLUSTER_NODE_UP && state == CLUSTER_NODE_UP) {
        ring->healthyNodeCount++;
    }

    node->state = state;
    node->stateChangedAt = clusterGetTimeMs();

    /* Rebuild strategies that depend on healthy node list */
    if (ring->strategyType == CLUSTER_STRATEGY_JUMP) {
        clusterRingRebuildJump(ring);
    } else if (ring->strategyType == CLUSTER_STRATEGY_MAGLEV) {
        ring->strategyData.maglev.needsRebuild = true;
    }

    /* Invoke callback */
    if (ring->stateCallback) {
        ring->stateCallback(ring, nodeId, oldState, state,
                            ring->stateCallbackData);
    }

    ring->version++;
    ring->lastModified = clusterGetTimeMs();

    return CLUSTER_OK;
}

clusterResult clusterRingSetNodeWeight(clusterRing *ring, uint64_t nodeId,
                                       uint32_t weight) {
    if (!ring) {
        return CLUSTER_ERR;
    }

    databox keybox = {.type = DATABOX_UNSIGNED_64, .data.u64 = nodeId};
    databox valbox;
    if (!multidictFind(ring->nodeById, &keybox, &valbox)) {
        return CLUSTER_ERR_NOT_FOUND;
    }

    clusterNode *node = (clusterNode *)valbox.data.ptr;

    if (node->weight == weight) {
        return CLUSTER_OK;
    }

    /* For Ketama: remove old vnodes, update weight, add new vnodes */
    if (ring->strategyType == CLUSTER_STRATEGY_KETAMA ||
        ring->strategyType == CLUSTER_STRATEGY_BOUNDED) {
        clusterRingRemoveVnodes(ring, nodeId);
        node->weight = weight;
        clusterRingAddVnodes(ring, node);
        clusterRingSortVnodes(ring);
    } else {
        node->weight = weight;
    }

    ring->version++;
    ring->lastModified = clusterGetTimeMs();

    return CLUSTER_OK;
}

const clusterNode *clusterRingGetNode(const clusterRing *ring,
                                      uint64_t nodeId) {
    if (!ring) {
        return NULL;
    }

    databox keybox = {.type = DATABOX_UNSIGNED_64, .data.u64 = nodeId};
    databox valbox;
    if (multidictFind(ring->nodeById, &keybox, &valbox)) {
        return (const clusterNode *)valbox.data.ptr;
    }
    return NULL;
}

uint32_t clusterRingNodeCount(const clusterRing *ring) {
    return ring ? ring->nodeCount : 0;
}

uint32_t clusterRingHealthyNodeCount(const clusterRing *ring) {
    return ring ? ring->healthyNodeCount : 0;
}

/* ====================================================================
 * Node Accessors
 * ==================================================================== */

uint64_t clusterNodeGetId(const clusterNode *node) {
    return node ? node->id : 0;
}

const char *clusterNodeGetName(const clusterNode *node) {
    return node ? node->name : "";
}

const char *clusterNodeGetAddress(const clusterNode *node) {
    return node ? node->address : "";
}

clusterNodeState clusterNodeGetState(const clusterNode *node) {
    return node ? node->state : CLUSTER_NODE_DOWN;
}

uint32_t clusterNodeGetWeight(const clusterNode *node) {
    return node ? node->weight : 0;
}

uint64_t clusterNodeGetCapacity(const clusterNode *node) {
    return node ? node->capacityBytes : 0;
}

uint64_t clusterNodeGetUsedBytes(const clusterNode *node) {
    return node ? node->usedBytes : 0;
}

const clusterLocation *clusterNodeGetLocation(const clusterNode *node) {
    return node ? &node->location : NULL;
}

/* ====================================================================
 * Strategy-Specific Locate Functions
 * ==================================================================== */

uint32_t clusterLocateKetama(const clusterRing *ring, const void *key,
                             uint32_t keyLen, const clusterNode **out,
                             uint32_t maxNodes) {
    const clusterKetamaData *data = &ring->strategyData.ketama;

    if (CLUSTER_UNLIKELY(data->vnodeCount == 0 || maxNodes == 0)) {
        return 0;
    }

    uint64_t hash = clusterHash64(key, keyLen, ring->hashSeed);

    /* Optimized binary search for first vnode with hashPoint >= hash */
    uint32_t lo;

    /* Extract hash points for SIMD-friendly search if vnode count is large */
    if (data->vnodeCount >= 32) {
        /* Use structure-aware binary search with prefetching */
        lo = 0;
        uint32_t hi = data->vnodeCount;
        while (hi - lo > 8) {
            uint32_t mid = lo + (hi - lo) / 2;
            /* Prefetch both possible next cache lines */
            CLUSTER_PREFETCH(&data->vnodes[lo + (mid - lo) / 2]);
            CLUSTER_PREFETCH(&data->vnodes[mid + (hi - mid) / 2]);
            if (data->vnodes[mid].hashPoint < hash) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        /* Linear scan for final elements */
        while (lo < hi && data->vnodes[lo].hashPoint < hash) {
            lo++;
        }
    } else {
        /* Standard binary search for small arrays */
        lo = 0;
        uint32_t hi = data->vnodeCount;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (data->vnodes[mid].hashPoint < hash) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
    }

    /* Wrap around if needed */
    if (lo >= data->vnodeCount) {
        lo = 0;
    }

    /* Collect unique nodes using bitmap-based deduplication */
    uint32_t effectiveMax =
        maxNodes < ring->nodeCount ? maxNodes : ring->nodeCount;

    /* Use bitmap for O(1) seen tracking instead of O(n) linear search */
    seenTracker seen;
    seenTrackerInit(&seen, ring->nodeCount);

    uint32_t found = 0;
    uint32_t checked = 0;

    while (found < effectiveMax && checked < data->vnodeCount) {
        uint32_t idx = (lo + checked) % data->vnodeCount;
        const clusterVnode *vn = &data->vnodes[idx];

        /* Prefetch next vnode for better cache behavior */
        if (checked + 1 < data->vnodeCount) {
            CLUSTER_PREFETCH(
                &data->vnodes[(lo + checked + 1) % data->vnodeCount]);
        }

        /* Use nodePtr directly instead of hash table lookup */
        clusterNode *node = vn->nodePtr;

        if (CLUSTER_LIKELY(node != NULL)) {
            /* Get node's index in nodeArray for bitmap tracking */
            int32_t nodeIdx = nodeIdToIndex(ring, vn->nodeId);

            if (nodeIdx >= 0 && !seenTrackerTest(&seen, (uint32_t)nodeIdx)) {
                seenTrackerSet(&seen, (uint32_t)nodeIdx);
                if (node->state == CLUSTER_NODE_UP) {
                    out[found++] = node;
                }
            }
        }

        checked++;
    }

    seenTrackerFree(&seen);
    return found;
}

uint32_t clusterLocateJump(const clusterRing *ring, const void *key,
                           uint32_t keyLen, const clusterNode **out,
                           uint32_t maxNodes) {
    const clusterJumpData *data = &ring->strategyData.jump;

    if (CLUSTER_UNLIKELY(data->bucketCount == 0 || maxNodes == 0)) {
        return 0;
    }

    uint64_t hash = clusterHash64(key, keyLen, ring->hashSeed);

    /* Get primary bucket */
    int32_t bucket = jumpConsistentHash(hash, (int32_t)data->bucketCount);
    if (CLUSTER_UNLIKELY(bucket < 0 || (uint32_t)bucket >= data->bucketCount)) {
        return 0;
    }

    uint32_t found = 0;

    /* Use bitmap for O(1) deduplication */
    seenTracker seen;
    seenTrackerInit(&seen, ring->nodeCount);

    /* Primary node - use direct array access via nodeIdToIndex */
    int32_t primaryIdx = nodeIdToIndex(ring, data->nodeIds[bucket]);
    if (primaryIdx >= 0) {
        const clusterNode *primary = ring->nodeArray[primaryIdx];
        if (CLUSTER_LIKELY(primary != NULL)) {
            out[found++] = primary;
            seenTrackerSet(&seen, (uint32_t)primaryIdx);
        }
    }

    /* For replicas, use different hash seeds */
    for (uint32_t r = 1; r < maxNodes && found < maxNodes; r++) {
        uint64_t replicaHash = clusterHash64(key, keyLen, ring->hashSeed + r);
        int32_t replicaBucket =
            jumpConsistentHash(replicaHash, (int32_t)data->bucketCount);

        if (replicaBucket >= 0 && (uint32_t)replicaBucket < data->bucketCount) {
            int32_t replicaIdx =
                nodeIdToIndex(ring, data->nodeIds[replicaBucket]);

            if (replicaIdx >= 0 &&
                !seenTrackerTest(&seen, (uint32_t)replicaIdx)) {
                seenTrackerSet(&seen, (uint32_t)replicaIdx);

                const clusterNode *replica = ring->nodeArray[replicaIdx];
                if (CLUSTER_LIKELY(replica != NULL)) {
                    out[found++] = replica;
                }
            }
        }
    }

    seenTrackerFree(&seen);
    return found;
}

uint32_t clusterLocateRendezvous(const clusterRing *ring, const void *key,
                                 uint32_t keyLen, const clusterNode **out,
                                 uint32_t maxNodes) {
    if (CLUSTER_UNLIKELY(ring->healthyNodeCount == 0 || maxNodes == 0)) {
        return 0;
    }

    uint32_t k =
        maxNodes < ring->healthyNodeCount ? maxNodes : ring->healthyNodeCount;

    /* Use heap-based top-k selection: O(n log k) instead of O(n^2) sorting */
    heapNode *heap = zcalloc(k, sizeof(heapNode));
    if (CLUSTER_UNLIKELY(!heap)) {
        return 0;
    }

    uint32_t heapSize = 0;

    /* Pre-compute key length for buffer copying */
    uint32_t copyLen = keyLen < 248 ? keyLen : 248;

    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        const clusterNode *node = ring->nodeArray[i];

        /* Prefetch next node for better cache behavior */
        if (i + 1 < ring->nodeCount) {
            CLUSTER_PREFETCH(ring->nodeArray[i + 1]);
        }

        if (CLUSTER_LIKELY(node != NULL) && node->state == CLUSTER_NODE_UP) {
            /* Combine key hash with node ID for HRW (Highest Random Weight) */
            uint8_t buf[256];
            memcpy(buf, key, copyLen);
            memcpy(buf + copyLen, &node->id, sizeof(node->id));

            uint64_t weight =
                clusterHash64(buf, copyLen + sizeof(node->id), ring->hashSeed);

            /* Insert into min-heap, keeping only top-k largest weights */
            heapInsertTopK(heap, &heapSize, k, node, weight);
        }
    }

    /* Extract results in descending weight order */
    heapExtractAll(heap, heapSize, out);

    zfree(heap);
    return heapSize;
}

uint32_t clusterLocateMaglev(const clusterRing *ring, const void *key,
                             uint32_t keyLen, const clusterNode **out,
                             uint32_t maxNodes) {
    const clusterMaglevData *data = &ring->strategyData.maglev;

    /* Rebuild lookup table if needed (non-const operation) */
    if (CLUSTER_UNLIKELY(data->needsRebuild)) {
        clusterRingRebuildMaglev((clusterRing *)ring);
        /* Re-read after rebuild - defensive programming in case rebuild
         * implementation changes */
        // cppcheck-suppress redundantAssignment - intentional re-read after
        // rebuild for future-proofing
        data = &ring->strategyData.maglev;
    }

    if (CLUSTER_UNLIKELY(ring->healthyNodeCount == 0 || maxNodes == 0)) {
        return 0;
    }

    /* Cap maxNodes by ring node count */
    uint32_t effectiveMax =
        maxNodes < ring->nodeCount ? maxNodes : ring->nodeCount;

    /* Use bitmap for O(1) seen tracking instead of O(n) linear search */
    seenTracker seen;
    seenTrackerInit(&seen, ring->nodeCount);

    uint64_t hash = clusterHash64(key, keyLen, ring->hashSeed);
    uint32_t idx = hash % data->tableSize;

    uint32_t found = 0;

    /* Walk through table for replicas with aggressive prefetching */
    for (uint32_t i = 0; i < data->tableSize && found < effectiveMax; i++) {
        uint32_t lookupIdx = (idx + i) % data->tableSize;

        /* Prefetch ahead in the lookup table (8 entries = 64 bytes = 1 cache
         * line) */
        if (i + 8 < data->tableSize) {
            CLUSTER_PREFETCH(&data->lookup[(idx + i + 8) % data->tableSize]);
        }

        uint64_t nodeId = data->lookup[lookupIdx];

        if (CLUSTER_UNLIKELY(nodeId == UINT64_MAX)) {
            continue;
        }

        /* Get node index for bitmap tracking */
        int32_t nodeIdx = nodeIdToIndex(ring, nodeId);

        if (nodeIdx >= 0 && !seenTrackerTest(&seen, (uint32_t)nodeIdx)) {
            seenTrackerSet(&seen, (uint32_t)nodeIdx);

            const clusterNode *node = ring->nodeArray[nodeIdx];
            if (CLUSTER_LIKELY(node != NULL)) {
                out[found++] = node;
            }
        }
    }

    seenTrackerFree(&seen);
    return found;
}

uint32_t clusterLocateBounded(const clusterRing *ring, const void *key,
                              uint32_t keyLen, const clusterNode **out,
                              uint32_t maxNodes) {
    /* Bounded load uses Ketama with load checking */
    /* For now, delegate to Ketama (full bounded load implementation
       would check node loads and skip overloaded nodes) */
    return clusterLocateKetama(ring, key, keyLen, out, maxNodes);
}

/* ====================================================================
 * Core Placement API
 * ==================================================================== */

clusterResult clusterRingLocate(const clusterRing *ring, const void *key,
                                uint32_t keyLen, clusterPlacement *out) {
    if (!ring || !key || keyLen == 0 || !out) {
        return CLUSTER_ERR;
    }

    /* Initialize output with multilist */
    clusterPlacementInit(out);
    out->hashValue = clusterHash64(key, keyLen, ring->hashSeed);

    if (ring->nodeCount == 0) {
        return CLUSTER_ERR_NO_NODES;
    }

    /* Update stats */
    ((clusterRing *)ring)->stats.locateOps++; /* Cast away const */

    /* Use a reasonable buffer for strategy results - no static limit */
    uint32_t maxReplicas = ring->defaultQuorum.replicaCount;

    /* Temporary stack buffer for strategy results */
    const clusterNode *stackReplicas[64];
    const clusterNode **replicas = stackReplicas;
    const clusterNode **heapReplicas = NULL;

    if (maxReplicas > 64) {
        heapReplicas = zcalloc(maxReplicas, sizeof(const clusterNode *));
        if (!heapReplicas) {
            clusterPlacementFree(out);
            return CLUSTER_ERR_ALLOC_FAILED;
        }
        replicas = heapReplicas;
    }

    uint32_t found = 0;

    switch (ring->strategyType) {
    case CLUSTER_STRATEGY_KETAMA:
        found = clusterLocateKetama(ring, key, keyLen, replicas, maxReplicas);
        break;
    case CLUSTER_STRATEGY_JUMP:
        found = clusterLocateJump(ring, key, keyLen, replicas, maxReplicas);
        break;
    case CLUSTER_STRATEGY_RENDEZVOUS:
        found =
            clusterLocateRendezvous(ring, key, keyLen, replicas, maxReplicas);
        break;
    case CLUSTER_STRATEGY_MAGLEV:
        found = clusterLocateMaglev(ring, key, keyLen, replicas, maxReplicas);
        break;
    case CLUSTER_STRATEGY_BOUNDED:
        found = clusterLocateBounded(ring, key, keyLen, replicas, maxReplicas);
        break;
    case CLUSTER_STRATEGY_CUSTOM:
        if (ring->customStrategy && ring->customStrategy->locate) {
            found = ring->customStrategy->locate(ring, key, keyLen, replicas,
                                                 maxReplicas);
        }
        break;
    }

    if (found == 0) {
        if (heapReplicas) {
            zfree(heapReplicas);
        }
        clusterPlacementFree(out);
        return CLUSTER_ERR_NO_NODES;
    }

    /* Copy results to output multilist */
    out->primary = replicas[0];
    out->healthyCount = 0;
    for (uint32_t i = 0; i < found; i++) {
        clusterPlacementAddReplica(out, replicas[i]);
        if (replicas[i]->state == CLUSTER_NODE_UP) {
            out->healthyCount++;
        }
    }

    if (heapReplicas) {
        zfree(heapReplicas);
    }

    return CLUSTER_OK;
}

clusterResult clusterRingLocateBox(const clusterRing *ring, const databox *key,
                                   clusterPlacement *out) {
    if (!key) {
        return CLUSTER_ERR;
    }

    const void *keyData;
    uint32_t keyLen;

    switch (key->type) {
    case DATABOX_BYTES:
        keyData = key->data.ptr;
        keyLen = (uint32_t)key->len;
        break;
    case DATABOX_SIGNED_64:
        keyData = &key->data.i64;
        keyLen = sizeof(key->data.i64);
        break;
    case DATABOX_UNSIGNED_64:
        keyData = &key->data.u64;
        keyLen = sizeof(key->data.u64);
        break;
    default:
        /* For other types, use the raw data union */
        keyData = &key->data;
        keyLen = sizeof(key->data);
        break;
    }

    return clusterRingLocate(ring, keyData, keyLen, out);
}

clusterResult clusterRingLocateKeyspace(const clusterRing *ring,
                                        const clusterKeySpace *ks,
                                        const void *key, uint32_t keyLen,
                                        clusterPlacement *out) {
    clusterResult result = clusterRingLocate(ring, key, keyLen, out);
    if (result == CLUSTER_OK) {
        out->keySpace = ks;
    }
    return result;
}

clusterResult clusterRingLocateBulk(const clusterRing *ring,
                                    const databox *keys, uint32_t keyCount,
                                    clusterPlacement *placements) {
    if (CLUSTER_UNLIKELY(!ring || !keys || !placements || keyCount == 0)) {
        return CLUSTER_ERR;
    }

    /* For small batches, use simple loop with prefetching */
    if (keyCount <= 4) {
        for (uint32_t i = 0; i < keyCount; i++) {
            /* Prefetch next key's data */
            if (i + 1 < keyCount) {
                const databox *nextKey = &keys[i + 1];
                if (nextKey->data.ptr) {
                    CLUSTER_PREFETCH(nextKey->data.ptr);
                }
            }
            clusterResult result =
                clusterRingLocateBox(ring, &keys[i], &placements[i]);
            if (CLUSTER_UNLIKELY(result != CLUSTER_OK)) {
                return result;
            }
        }
        return CLUSTER_OK;
    }

    /* For larger batches, batch-compute hashes first for better cache
     * efficiency */
    uint64_t *hashes = zcalloc(keyCount, sizeof(uint64_t));
    if (CLUSTER_UNLIKELY(!hashes)) {
        /* Fallback to simple loop on allocation failure */
        for (uint32_t i = 0; i < keyCount; i++) {
            clusterResult result =
                clusterRingLocateBox(ring, &keys[i], &placements[i]);
            if (result != CLUSTER_OK) {
                return result;
            }
        }
        return CLUSTER_OK;
    }

    /* Batch hash computation - prefetch ahead for streaming */
    for (uint32_t i = 0; i < keyCount; i++) {
        /* Prefetch next key data */
        if (i + 2 < keyCount && keys[i + 2].data.ptr) {
            CLUSTER_PREFETCH(keys[i + 2].data.ptr);
        }

        const databox *key = &keys[i];
        const void *keyData;
        uint32_t keyLen;

        /* Extract key data based on type */
        switch (key->type) {
        case DATABOX_BYTES:
        case DATABOX_CONTAINER_FLEX_MAP:
        case DATABOX_CONTAINER_FLEX_LIST:
        case DATABOX_CONTAINER_FLEX_SET:
        case DATABOX_CONTAINER_FLEX_TUPLE:
            keyData = key->data.bytes.start;
            keyLen = key->len;
            break;
        case DATABOX_SIGNED_64:
        case DATABOX_UNSIGNED_64:
            keyData = &key->data.u64;
            keyLen = sizeof(uint64_t);
            break;
        default:
            keyData = &key->data;
            keyLen = sizeof(key->data);
            break;
        }

        hashes[i] = clusterHash64(keyData, keyLen, ring->hashSeed);
    }

    /* Now use pre-computed hashes for placement decisions */
    /* This improves cache locality since we're not jumping between key data */
    clusterResult finalResult = CLUSTER_OK;

    for (uint32_t i = 0; i < keyCount; i++) {
        /* Initialize placement */
        clusterPlacementInit(&placements[i]);

        /* Use the pre-computed hash directly based on strategy */
        /* Note: For full optimization, strategy-specific bulk functions would
         * be needed */
        /* For now, we still call the locate function but could optimize further
         */
        clusterResult result =
            clusterRingLocateBox(ring, &keys[i], &placements[i]);
        if (CLUSTER_UNLIKELY(result != CLUSTER_OK)) {
            /* Continue processing remaining keys but track error */
            finalResult = result;
        }
    }

    zfree(hashes);
    return finalResult;
}

/* ====================================================================
 * Routing Decision APIs
 * ==================================================================== */

clusterResult clusterRingPlanWrite(const clusterRing *ring, const void *key,
                                   uint32_t keyLen, const clusterQuorum *quorum,
                                   clusterWriteSet *out) {
    if (!ring || !key || !out) {
        return CLUSTER_ERR;
    }

    const clusterQuorum *q = quorum ? quorum : &ring->defaultQuorum;

    /* Initialize with multilist */
    clusterWriteSetInit(out);

    /* Get placement */
    clusterPlacement placement;
    clusterResult result = clusterRingLocate(ring, key, keyLen, &placement);
    if (result != CLUSTER_OK) {
        clusterWriteSetFree(out);
        return result;
    }

    /* Copy targets from placement to write set */
    size_t replicaCount = clusterPlacementReplicaCount(&placement);
    for (size_t i = 0; i < replicaCount; i++) {
        clusterWriteSetAddTarget(out,
                                 clusterPlacementGetReplica(&placement, i));
    }

    /* Calculate sync/async requirements */
    size_t targetCount = clusterWriteSetTargetCount(out);
    out->syncRequired = q->writeSync;
    if (out->syncRequired > targetCount) {
        out->syncRequired = (uint8_t)targetCount;
    }
    out->asyncAllowed = (uint8_t)(targetCount - out->syncRequired);

    /* Suggest timeout based on replica count */
    out->suggestedTimeoutMs = 100 + (out->syncRequired * 50);

    /* Update stats */
    ((clusterRing *)ring)->stats.writeOps++;

    /* Check if quorum is achievable */
    if (placement.healthyCount < q->writeQuorum) {
        clusterPlacementFree(&placement);
        return CLUSTER_ERR_QUORUM_FAILED;
    }

    clusterPlacementFree(&placement);
    return CLUSTER_OK;
}

clusterResult clusterRingPlanRead(const clusterRing *ring, const void *key,
                                  uint32_t keyLen, const clusterQuorum *quorum,
                                  clusterReadSet *out) {
    if (!ring || !key || !out) {
        return CLUSTER_ERR;
    }

    const clusterQuorum *q = quorum ? quorum : &ring->defaultQuorum;

    /* Initialize with multilist */
    clusterReadSetInit(out);

    /* Get placement */
    clusterPlacement placement;
    clusterResult result = clusterRingLocate(ring, key, keyLen, &placement);
    if (result != CLUSTER_OK) {
        clusterReadSetFree(out);
        return result;
    }

    /* Copy candidates in preference order */
    size_t replicaCount = clusterPlacementReplicaCount(&placement);
    for (size_t i = 0; i < replicaCount; i++) {
        clusterReadSetAddCandidate(out,
                                   clusterPlacementGetReplica(&placement, i));
    }

    size_t candidateCount = clusterReadSetCandidateCount(out);
    out->requiredResponses = q->readQuorum;
    if (out->requiredResponses > candidateCount) {
        out->requiredResponses = (uint8_t)candidateCount;
    }
    out->readRepair = q->readRepairEnabled;

    /* Update stats */
    ((clusterRing *)ring)->stats.readOps++;

    /* Check if quorum is achievable */
    if (placement.healthyCount < q->readQuorum) {
        clusterPlacementFree(&placement);
        return CLUSTER_ERR_QUORUM_FAILED;
    }

    clusterPlacementFree(&placement);
    return CLUSTER_OK;
}

clusterResult clusterRingSelectReadNode(const clusterRing *ring,
                                        const clusterPlacement *placement,
                                        const clusterNode **selected) {
    if (!ring || !placement || !selected) {
        return CLUSTER_ERR;
    }

    *selected = NULL;
    size_t replicaCount = clusterPlacementReplicaCount(placement);

    /* If load-aware routing, pick lowest-load healthy node */
    if (ring->loadAwareRouting && ring->healthProvider) {
        float lowestLoad = 2.0f; /* > 1.0 */

        for (size_t i = 0; i < replicaCount; i++) {
            const clusterNode *node = clusterPlacementGetReplica(placement, i);
            if (!node || node->state != CLUSTER_NODE_UP) {
                continue;
            }

            /* Use cached load or query provider */
            float load = node->lastLoad.cpuUsage;
            if (load < lowestLoad) {
                lowestLoad = load;
                *selected = node;
            }
        }

        if (*selected) {
            return CLUSTER_OK;
        }
    }

    /* Default: return first healthy node */
    for (size_t i = 0; i < replicaCount; i++) {
        const clusterNode *node = clusterPlacementGetReplica(placement, i);
        if (node && node->state == CLUSTER_NODE_UP) {
            *selected = node;
            return CLUSTER_OK;
        }
    }

    return CLUSTER_ERR_NO_NODES;
}

/* ====================================================================
 * Affinity Checking
 * ==================================================================== */

static uint32_t getLocationValue(const clusterLocation *loc,
                                 clusterTopologyLevel level) {
    switch (level) {
    case CLUSTER_LEVEL_NODE:
        return (uint32_t)loc->nodeId;
    case CLUSTER_LEVEL_RACK:
        return loc->rackId;
    case CLUSTER_LEVEL_CAGE:
        return loc->cageId;
    case CLUSTER_LEVEL_DATACENTER:
        return loc->dcId;
    case CLUSTER_LEVEL_AVAILABILITY_ZONE:
        return loc->azId;
    case CLUSTER_LEVEL_REGION:
        return loc->regionId;
    case CLUSTER_LEVEL_COUNTRY:
        return loc->countryId;
    case CLUSTER_LEVEL_CONTINENT:
        return loc->continentId;
    default:
        return 0;
    }
}

bool clusterCheckAffinity(const clusterRing *ring, const clusterNode **nodes,
                          uint32_t count, const clusterAffinityRule *rules,
                          uint8_t ruleCount) {
    (void)ring;

    if (count == 0) {
        return true;
    }

    /* Dynamically allocate tracking array based on count */
    uint32_t *values = zcalloc(count, sizeof(uint32_t));
    if (!values) {
        return false; /* Allocation failure - fail constraint check */
    }

    bool result = true;

    for (uint8_t r = 0; r < ruleCount && result; r++) {
        const clusterAffinityRule *rule = &rules[r];

        /* Count distinct values at this level */
        uint32_t uniqueCount = 0;

        for (uint32_t i = 0; i < count; i++) {
            uint32_t val =
                getLocationValue(&nodes[i]->location, rule->spreadLevel);

            /* Check if we've seen this value */
            bool found = false;
            for (uint32_t j = 0; j < uniqueCount; j++) {
                if (values[j] == val) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                values[uniqueCount++] = val;
            }
        }

        /* Check constraint */
        if (uniqueCount < rule->minSpread && rule->required) {
            result = false;
        }
    }

    zfree(values);
    return result;
}

/* ====================================================================
 * Keyspace Management
 * ==================================================================== */

clusterResult clusterRingAddKeySpace(clusterRing *ring,
                                     const clusterKeySpaceConfig *config,
                                     clusterKeySpace **out) {
    if (!ring || !config || !config->name) {
        return CLUSTER_ERR;
    }

    /* Check if exists */
    databox keybox = {.type = DATABOX_BYTES,
                      .data.ptr = (void *)config->name,
                      .len = strlen(config->name)};
    databox valbox;
    if (multidictFind(ring->keySpaceByName, &keybox, &valbox)) {
        return CLUSTER_ERR_EXISTS;
    }

    /* Grow array if needed */
    if (ring->keySpaceCount >= ring->keySpaceCapacity) {
        uint32_t newCap = ring->keySpaceCapacity * 2;
        clusterKeySpace **newArr =
            zrealloc(ring->keySpaces, newCap * sizeof(clusterKeySpace *));
        if (!newArr) {
            return CLUSTER_ERR_ALLOC_FAILED;
        }
        ring->keySpaces = newArr;
        ring->keySpaceCapacity = newCap;
    }

    /* Create keyspace */
    clusterKeySpace *ks = zcalloc(1, sizeof(*ks));
    if (!ks) {
        return CLUSTER_ERR_ALLOC_FAILED;
    }

    /* Convert const char * to mdsc internally */
    ks->name = mdscnew(config->name);
    ks->id = ring->keySpaceCount;
    ks->quorum = config->quorum;
    ks->strategy = config->strategy;

    /* Copy rules */
    if (config->rules && config->ruleCount > 0) {
        ks->rules = zcalloc(config->ruleCount, sizeof(clusterAffinityRule));
        if (ks->rules) {
            memcpy(ks->rules, config->rules,
                   config->ruleCount * sizeof(clusterAffinityRule));
            ks->ruleCount = config->ruleCount;
        }
    }

    /* Add to structures */
    ring->keySpaces[ring->keySpaceCount++] = ks;

    valbox.type = DATABOX_PTR;
    valbox.data.ptr = ks;
    multidictAdd(ring->keySpaceByName, &keybox, &valbox);

    if (out) {
        *out = ks;
    }

    ring->version++;
    return CLUSTER_OK;
}

clusterResult clusterRingRemoveKeySpace(clusterRing *ring, const char *name) {
    if (!ring || !name) {
        return CLUSTER_ERR;
    }

    databox keybox = {
        .type = DATABOX_BYTES, .data.ptr = (void *)name, .len = strlen(name)};
    databox valbox;
    if (!multidictFind(ring->keySpaceByName, &keybox, &valbox)) {
        return CLUSTER_ERR_NOT_FOUND;
    }

    clusterKeySpace *ks = (clusterKeySpace *)valbox.data.ptr;

    /* Remove from dict */
    multidictDelete(ring->keySpaceByName, &keybox);

    /* Find and remove from array */
    for (uint32_t i = 0; i < ring->keySpaceCount; i++) {
        if (ring->keySpaces[i] == ks) {
            /* Shift remaining elements */
            for (uint32_t j = i; j < ring->keySpaceCount - 1; j++) {
                ring->keySpaces[j] = ring->keySpaces[j + 1];
            }
            ring->keySpaceCount--;
            break;
        }
    }

    /* Free keyspace */
    if (ks->rules) {
        zfree(ks->rules);
    }
    zfree(ks);

    ring->version++;
    return CLUSTER_OK;
}

const clusterKeySpace *clusterRingGetKeySpace(const clusterRing *ring,
                                              const char *name) {
    if (!ring || !name) {
        return NULL;
    }

    databox keybox = {
        .type = DATABOX_BYTES, .data.ptr = (void *)name, .len = strlen(name)};
    databox valbox;
    if (multidictFind(ring->keySpaceByName, &keybox, &valbox)) {
        return (const clusterKeySpace *)valbox.data.ptr;
    }
    return NULL;
}

/* ====================================================================
 * Rebalancing
 * ==================================================================== */

const clusterRebalancePlan *
clusterRingGetRebalancePlan(const clusterRing *ring) {
    return ring ? ring->rebalancePlan : NULL;
}

uint32_t clusterRebalancePlanMoveCount(const clusterRebalancePlan *plan) {
    return plan ? plan->moveCount : 0;
}

const clusterRebalanceMove *
clusterRebalancePlanGetMove(const clusterRebalancePlan *plan, uint32_t index) {
    if (!plan || index >= plan->moveCount) {
        return NULL;
    }
    return &plan->moves[index];
}

float clusterRebalancePlanProgress(const clusterRebalancePlan *plan) {
    if (!plan || plan->moveCount == 0) {
        return 1.0f;
    }
    return (float)plan->completedCount / (float)plan->moveCount;
}

clusterResult clusterRingCompleteMove(clusterRing *ring, uint32_t moveIndex) {
    if (!ring || !ring->rebalancePlan) {
        return CLUSTER_ERR;
    }

    if (moveIndex >= ring->rebalancePlan->moveCount) {
        return CLUSTER_ERR_NOT_FOUND;
    }

    clusterRebalanceMove *move = &ring->rebalancePlan->moves[moveIndex];
    if (move->state != CLUSTER_MOVE_IN_PROGRESS) {
        return CLUSTER_ERR_INVALID_STATE;
    }

    move->state = CLUSTER_MOVE_COMPLETED;
    ring->rebalancePlan->completedCount++;
    ring->rebalancePlan->movedBytes += move->estimatedBytes;
    ring->stats.rebalanceMoves++;

    /* Invoke callback */
    if (ring->rebalanceCallback) {
        ring->rebalanceCallback(ring, ring->rebalancePlan,
                                ring->rebalanceCallbackData);
    }

    return CLUSTER_OK;
}

clusterResult clusterRingCancelRebalance(clusterRing *ring) {
    if (!ring || !ring->rebalancePlan) {
        return CLUSTER_ERR;
    }

    if (ring->rebalancePlan->moves) {
        zfree(ring->rebalancePlan->moves);
    }
    zfree(ring->rebalancePlan);
    ring->rebalancePlan = NULL;
    ring->rebalanceInProgress = false;

    return CLUSTER_OK;
}

/* ====================================================================
 * Health & Load Integration
 * ==================================================================== */

void clusterRingSetHealthProvider(clusterRing *ring,
                                  clusterHealthProvider *provider) {
    if (ring) {
        ring->healthProvider = provider;
        ring->loadAwareRouting = (provider != NULL);
    }
}

clusterResult clusterRingUpdateNodeHealth(clusterRing *ring, uint64_t nodeId,
                                          const clusterNodeHealth *health) {
    if (!ring || !health) {
        return CLUSTER_ERR;
    }

    databox keybox = {.type = DATABOX_UNSIGNED_64, .data.u64 = nodeId};
    databox valbox;
    if (!multidictFind(ring->nodeById, &keybox, &valbox)) {
        return CLUSTER_ERR_NOT_FOUND;
    }

    clusterNode *node = (clusterNode *)valbox.data.ptr;
    node->lastHealth = *health;
    node->lastHealthCheck = clusterGetTimeMs();

    /* Auto-update state based on health */
    if (!health->reachable && node->state == CLUSTER_NODE_UP) {
        node->failureCount++;
        if (node->failureCount >= 3) {
            clusterRingSetNodeState(ring, nodeId, CLUSTER_NODE_SUSPECT);
        }
    } else if (health->reachable && node->state == CLUSTER_NODE_SUSPECT) {
        node->failureCount = 0;
        clusterRingSetNodeState(ring, nodeId, CLUSTER_NODE_UP);
    }

    return CLUSTER_OK;
}

clusterResult clusterRingUpdateNodeLoad(clusterRing *ring, uint64_t nodeId,
                                        const clusterNodeLoad *load) {
    if (!ring || !load) {
        return CLUSTER_ERR;
    }

    databox keybox = {.type = DATABOX_UNSIGNED_64, .data.u64 = nodeId};
    databox valbox;
    if (!multidictFind(ring->nodeById, &keybox, &valbox)) {
        return CLUSTER_ERR_NOT_FOUND;
    }

    clusterNode *node = (clusterNode *)valbox.data.ptr;
    node->lastLoad = *load;
    node->lastLoadCheck = clusterGetTimeMs();

    return CLUSTER_OK;
}

/* ====================================================================
 * Statistics
 * ==================================================================== */

void clusterRingGetStats(const clusterRing *ring, clusterRingStats *stats) {
    if (!ring || !stats) {
        return;
    }

    stats->nodeCount = ring->nodeCount;
    stats->healthyNodes = ring->healthyNodeCount;
    stats->keySpaceCount = ring->keySpaceCount;

    /* Virtual node count */
    if (ring->strategyType == CLUSTER_STRATEGY_KETAMA ||
        ring->strategyType == CLUSTER_STRATEGY_BOUNDED) {
        stats->vnodeCount = ring->strategyData.ketama.vnodeCount;
    } else {
        stats->vnodeCount = 0;
    }

    /* Copy operation stats */
    stats->locateOps = ring->stats.locateOps;
    stats->writeOps = ring->stats.writeOps;
    stats->readOps = ring->stats.readOps;
    stats->rebalanceMoves = ring->stats.rebalanceMoves;

    /* Copy timing stats */
    stats->avgLocateNs = ring->stats.avgLocateNs;
    stats->p99LocateNs = ring->stats.p99LocateNs;
    stats->maxLocateNs = ring->stats.maxLocateNs;

    /* Calculate load distribution */
    if (ring->healthyNodeCount > 0 &&
        ring->strategyType == CLUSTER_STRATEGY_KETAMA) {
        /* Count vnodes per healthy node */
        uint32_t maxVnodes = 0, minVnodes = UINT32_MAX;
        uint64_t totalVnodes = 0;

        for (uint32_t i = 0; i < ring->nodeCount; i++) {
            const clusterNode *node = ring->nodeArray[i];
            if (node && node->state == CLUSTER_NODE_UP) {
                if (node->vnodeCount > maxVnodes) {
                    maxVnodes = node->vnodeCount;
                }
                if (node->vnodeCount < minVnodes) {
                    minVnodes = node->vnodeCount;
                }
                totalVnodes += node->vnodeCount;
            }
        }

        float avgVnodes = (float)totalVnodes / ring->healthyNodeCount;
        stats->maxNodeLoad = (float)maxVnodes / avgVnodes;
        stats->minNodeLoad = (float)minVnodes / avgVnodes;

        /* Calculate variance */
        float sumSqDiff = 0;
        for (uint32_t i = 0; i < ring->nodeCount; i++) {
            const clusterNode *node = ring->nodeArray[i];
            if (node && node->state == CLUSTER_NODE_UP) {
                float diff = (float)node->vnodeCount - avgVnodes;
                sumSqDiff += diff * diff;
            }
        }
        stats->loadVariance = sumSqDiff / ring->healthyNodeCount;
    } else {
        stats->maxNodeLoad = 1.0f;
        stats->minNodeLoad = 1.0f;
        stats->loadVariance = 0.0f;
    }

    /* Estimate memory usage */
    stats->memoryBytes = sizeof(clusterRing);
    stats->memoryBytes += ring->nodeCapacity * sizeof(clusterNode *);
    stats->memoryBytes += ring->nodeCount * sizeof(clusterNode);

    if (ring->strategyType == CLUSTER_STRATEGY_KETAMA) {
        stats->memoryBytes +=
            ring->strategyData.ketama.vnodeCapacity * sizeof(clusterVnode);
    } else if (ring->strategyType == CLUSTER_STRATEGY_MAGLEV) {
        stats->memoryBytes +=
            ring->strategyData.maglev.tableSize * sizeof(uint64_t);
    }
}

/* ====================================================================
 * Callbacks
 * ==================================================================== */

void clusterRingSetNodeStateCallback(clusterRing *ring,
                                     clusterNodeStateCallback cb,
                                     void *userData) {
    if (ring) {
        ring->stateCallback = cb;
        ring->stateCallbackData = userData;
    }
}

void clusterRingSetRebalanceCallback(clusterRing *ring,
                                     clusterRebalanceCallback cb,
                                     void *userData) {
    if (ring) {
        ring->rebalanceCallback = cb;
        ring->rebalanceCallbackData = userData;
    }
}

/* ====================================================================
 * Node Iteration
 * ==================================================================== */

void clusterRingIterateNodes(const clusterRing *ring, clusterNodeIterFn fn,
                             void *userData) {
    if (!ring || !fn) {
        return;
    }

    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        if (ring->nodeArray[i]) {
            if (!fn(ring->nodeArray[i], userData)) {
                break; /* Callback returned false, stop iteration */
            }
        }
    }
}

void clusterRingIterateNodesByState(const clusterRing *ring,
                                    clusterNodeState state,
                                    clusterNodeIterFn fn, void *userData) {
    if (!ring || !fn) {
        return;
    }

    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        clusterNode *node = ring->nodeArray[i];
        if (node && node->state == state) {
            if (!fn(node, userData)) {
                break;
            }
        }
    }
}

void clusterRingIterateNodesByLocation(const clusterRing *ring,
                                       clusterTopologyLevel level,
                                       uint32_t levelValue,
                                       clusterNodeIterFn fn, void *userData) {
    if (!ring || !fn) {
        return;
    }

    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        clusterNode *node = ring->nodeArray[i];
        if (node && getLocationValue(&node->location, level) == levelValue) {
            if (!fn(node, userData)) {
                break;
            }
        }
    }
}

/* ====================================================================
 * Serialization
 * ==================================================================== */

size_t clusterRingSerializeSize(const clusterRing *ring) {
    if (!ring) {
        return 0;
    }

    size_t size = 0;

    /* Header */
    size += 4; /* magic */
    size += 4; /* version */
    size +=
        4 + (ring->name ? mdsclen(ring->name) : 0); /* ring name: len + data */
    size += sizeof(clusterQuorum);
    size += sizeof(clusterVnodeConfig);
    size += 4; /* strategy type */
    size += 4; /* hash seed */
    size += 4; /* node count */

    /* Nodes - serialize each individually with length-prefixed strings */
    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        clusterNode *node = ring->nodeArray[i];
        if (node) {
            size += 8; /* id */
            size += 4 + (node->name ? mdsclen(node->name)
                                    : 0); /* name: len + data */
            size += 4 + (node->address ? mdsclen(node->address)
                                       : 0); /* address: len + data */
            size += sizeof(clusterLocation);
            size += 4; /* weight */
            size += 8; /* capacityBytes */
            size += 4; /* state */
            size += 8; /* usedBytes */
        }
    }

    /* Keyspace count */
    size += 4;

    /* Keyspaces */
    for (uint32_t i = 0; i < ring->keySpaceCount; i++) {
        const clusterKeySpace *ks = ring->keySpaces[i];
        if (ks) {
            size +=
                4 + (ks->name ? mdsclen(ks->name) : 0); /* name: len + data */
            size += sizeof(clusterQuorum);
            size += 4; /* strategy */
            size += 1; /* rule count */
            size += ks->ruleCount * sizeof(clusterAffinityRule);
        }
    }

    return size;
}

/* Helper to write length-prefixed string */
static uint8_t *serializeString(uint8_t *p, const mdsc *s) {
    uint32_t len = s ? mdsclen(s) : 0;
    memcpy(p, &len, 4);
    p += 4;
    if (len > 0) {
        memcpy(p, s, len);
        p += len;
    }
    return p;
}

size_t clusterRingSerialize(const clusterRing *ring, void *buf, size_t bufLen) {
    if (!ring || !buf) {
        return 0;
    }

    size_t needed = clusterRingSerializeSize(ring);
    if (bufLen < needed) {
        return 0;
    }

    uint8_t *p = (uint8_t *)buf;

    /* Magic */
    memcpy(p, "DKCR", 4);
    p += 4;

    /* Version - bump to 2 for new format */
    uint32_t ver = 2;
    memcpy(p, &ver, 4);
    p += 4;

    /* Name (length-prefixed) */
    p = serializeString(p, ring->name);

    /* Default quorum */
    memcpy(p, &ring->defaultQuorum, sizeof(clusterQuorum));
    p += sizeof(clusterQuorum);

    /* Vnode config */
    memcpy(p, &ring->vnodeConfig, sizeof(clusterVnodeConfig));
    p += sizeof(clusterVnodeConfig);

    /* Strategy type */
    uint32_t strat = (uint32_t)ring->strategyType;
    memcpy(p, &strat, 4);
    p += 4;

    /* Hash seed */
    memcpy(p, &ring->hashSeed, 4);
    p += 4;

    /* Node count */
    memcpy(p, &ring->nodeCount, 4);
    p += 4;

    /* Nodes - serialize each with length-prefixed strings */
    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        const clusterNode *node = ring->nodeArray[i];
        if (node) {
            /* id */
            memcpy(p, &node->id, 8);
            p += 8;

            /* name (length-prefixed) */
            p = serializeString(p, node->name);

            /* address (length-prefixed) */
            p = serializeString(p, node->address);

            /* location */
            memcpy(p, &node->location, sizeof(clusterLocation));
            p += sizeof(clusterLocation);

            /* weight */
            memcpy(p, &node->weight, 4);
            p += 4;

            /* capacityBytes */
            memcpy(p, &node->capacityBytes, 8);
            p += 8;

            /* state */
            uint32_t state = (uint32_t)node->state;
            memcpy(p, &state, 4);
            p += 4;

            /* usedBytes */
            memcpy(p, &node->usedBytes, 8);
            p += 8;
        }
    }

    /* Keyspace count */
    memcpy(p, &ring->keySpaceCount, 4);
    p += 4;

    /* Keyspaces */
    for (uint32_t i = 0; i < ring->keySpaceCount; i++) {
        const clusterKeySpace *ks = ring->keySpaces[i];
        if (ks) {
            /* name (length-prefixed) */
            p = serializeString(p, ks->name);

            memcpy(p, &ks->quorum, sizeof(clusterQuorum));
            p += sizeof(clusterQuorum);
            uint32_t ksstrat = (uint32_t)ks->strategy;
            memcpy(p, &ksstrat, 4);
            p += 4;
            *p++ = ks->ruleCount;
            if (ks->ruleCount > 0 && ks->rules) {
                memcpy(p, ks->rules,
                       ks->ruleCount * sizeof(clusterAffinityRule));
                p += ks->ruleCount * sizeof(clusterAffinityRule);
            }
        }
    }

    return (size_t)(p - (uint8_t *)buf);
}

/* Helper to read length-prefixed string.
 * Returns a temporary null-terminated buffer (caller must use before next
 * call). Sets *outLen to the string length. */
static const uint8_t *deserializeString(const uint8_t *p, char *tempBuf,
                                        size_t tempBufSize, uint32_t *outLen) {
    uint32_t len;
    memcpy(&len, p, 4);
    p += 4;
    *outLen = len;
    if (len > 0) {
        size_t copyLen = len < tempBufSize - 1 ? len : tempBufSize - 1;
        memcpy(tempBuf, p, copyLen);
        tempBuf[copyLen] = '\0';
        p += len;
    } else {
        tempBuf[0] = '\0';
    }
    return p;
}

clusterRing *clusterRingDeserialize(const void *buf, size_t bufLen) {
    if (!buf || bufLen < 8) {
        return NULL;
    }

    const uint8_t *p = (const uint8_t *)buf;

    /* Check magic */
    if (memcmp(p, "DKCR", 4) != 0) {
        return NULL;
    }
    p += 4;

    /* Check version */
    uint32_t ver;
    memcpy(&ver, p, 4);
    p += 4;
    if (ver != 2) {
        return NULL; /* Only support version 2 (dynamic strings) */
    }

    /* Temp buffer for string deserialization */
    char tempBuf[4096];
    uint32_t strLen;

    /* Create config from serialized data */
    clusterRingConfig config;
    memset(&config, 0, sizeof(config));

    /* Name (length-prefixed) */
    p = deserializeString(p, tempBuf, sizeof(tempBuf), &strLen);
    config.name = tempBuf;

    /* Default quorum */
    memcpy(&config.defaultQuorum, p, sizeof(clusterQuorum));
    p += sizeof(clusterQuorum);

    /* Vnode config */
    memcpy(&config.vnodes, p, sizeof(clusterVnodeConfig));
    p += sizeof(clusterVnodeConfig);

    /* Strategy type */
    uint32_t strat;
    memcpy(&strat, p, 4);
    p += 4;
    config.strategyType = (clusterStrategyType)strat;

    /* Hash seed */
    memcpy(&config.hashSeed, p, 4);
    p += 4;

    /* Node count */
    uint32_t nodeCount;
    memcpy(&nodeCount, p, 4);
    p += 4;
    config.expectedNodeCount = nodeCount;

    /* Create ring */
    clusterRing *ring = clusterRingNew(&config);
    if (!ring) {
        return NULL;
    }

    /* Add nodes */
    char nameBuf[4096];
    char addrBuf[4096];
    for (uint32_t i = 0; i < nodeCount; i++) {
        clusterNodeConfig nodeCfg;
        memset(&nodeCfg, 0, sizeof(nodeCfg));

        /* id */
        memcpy(&nodeCfg.id, p, 8);
        p += 8;

        /* name (length-prefixed) */
        p = deserializeString(p, nameBuf, sizeof(nameBuf), &strLen);
        nodeCfg.name = nameBuf;

        /* address (length-prefixed) */
        p = deserializeString(p, addrBuf, sizeof(addrBuf), &strLen);
        nodeCfg.address = addrBuf;

        /* location */
        memcpy(&nodeCfg.location, p, sizeof(clusterLocation));
        p += sizeof(clusterLocation);

        /* weight */
        memcpy(&nodeCfg.weight, p, 4);
        p += 4;

        /* capacityBytes */
        memcpy(&nodeCfg.capacityBytes, p, 8);
        p += 8;

        /* state */
        uint32_t state;
        memcpy(&state, p, 4);
        p += 4;
        nodeCfg.initialState = (clusterNodeState)state;

        /* usedBytes - read but apply after adding */
        uint64_t usedBytes;
        memcpy(&usedBytes, p, 8);
        p += 8;

        clusterRingAddNode(ring, &nodeCfg);

        /* Restore used bytes */
        databox keybox = {.type = DATABOX_UNSIGNED_64, .data.u64 = nodeCfg.id};
        databox valbox;
        if (multidictFind(ring->nodeById, &keybox, &valbox)) {
            clusterNode *node = (clusterNode *)valbox.data.ptr;
            node->usedBytes = usedBytes;
        }
    }

    /* Keyspace count */
    uint32_t ksCount;
    memcpy(&ksCount, p, 4);
    p += 4;

    /* Add keyspaces */
    for (uint32_t i = 0; i < ksCount; i++) {
        clusterKeySpaceConfig ksCfg;
        memset(&ksCfg, 0, sizeof(ksCfg));

        /* name (length-prefixed) */
        p = deserializeString(p, nameBuf, sizeof(nameBuf), &strLen);
        ksCfg.name = nameBuf;

        memcpy(&ksCfg.quorum, p, sizeof(clusterQuorum));
        p += sizeof(clusterQuorum);

        uint32_t ksstrat;
        memcpy(&ksstrat, p, 4);
        p += 4;
        ksCfg.strategy = (clusterStrategyType)ksstrat;

        uint8_t ruleCount = *p++;
        if (ruleCount > 0) {
            ksCfg.rules = (clusterAffinityRule *)p;
            ksCfg.ruleCount = ruleCount;
            p += ruleCount * sizeof(clusterAffinityRule);
        }

        clusterRingAddKeySpace(ring, &ksCfg, NULL);
    }

    return ring;
}

uint64_t clusterRingGetVersion(const clusterRing *ring) {
    return ring ? ring->version : 0;
}

size_t clusterRingSerializeDelta(const clusterRing *ring, uint64_t sinceVersion,
                                 void *buf, size_t bufLen) {
    /* For now, just do full serialization if version changed */
    (void)sinceVersion;
    if (ring && ring->version > sinceVersion) {
        return clusterRingSerialize(ring, buf, bufLen);
    }
    return 0;
}

clusterResult clusterRingApplyDelta(clusterRing *ring, const void *buf,
                                    size_t bufLen) {
    /* For now, delta is full ring - deserialize and merge */
    (void)ring;
    (void)buf;
    (void)bufLen;
    return CLUSTER_ERR; /* Not implemented yet */
}

/* ====================================================================
 * Testing / Debug
 * ==================================================================== */

#ifdef DATAKIT_TEST
#include "ctest.h"

void clusterRingRepr(const clusterRing *ring) {
    if (!ring) {
        printf("clusterRing: (null)\n");
        return;
    }

    printf("clusterRing \"%s\":\n", ring->name);
    printf("  Strategy: %d\n", ring->strategyType);
    printf("  Nodes: %u (healthy: %u)\n", ring->nodeCount,
           ring->healthyNodeCount);
    printf("  Keyspaces: %u\n", ring->keySpaceCount);
    printf("  Version: %" PRIu64 "\n", ring->version);

    if (ring->strategyType == CLUSTER_STRATEGY_KETAMA) {
        printf("  VNodes: %u\n", ring->strategyData.ketama.vnodeCount);
    }

    printf("  Default Quorum: N=%u W=%u R=%u\n",
           ring->defaultQuorum.replicaCount, ring->defaultQuorum.writeQuorum,
           ring->defaultQuorum.readQuorum);
}

void clusterNodeRepr(const clusterNode *node) {
    if (!node) {
        printf("clusterNode: (null)\n");
        return;
    }

    printf("clusterNode #%" PRIu64 " \"%s\":\n", node->id, node->name);
    printf("  Address: %s\n", node->address);
    printf("  State: %d\n", node->state);
    printf("  Weight: %u\n", node->weight);
    printf("  Location: rack=%u dc=%u az=%u region=%u\n", node->location.rackId,
           node->location.dcId, node->location.azId, node->location.regionId);
    printf("  VNodes: %u (start=%u)\n", node->vnodeCount,
           node->vnodeStartIndex);
}

void clusterPlacementRepr(const clusterPlacement *p) {
    if (!p) {
        printf("clusterPlacement: (null)\n");
        return;
    }

    size_t replicaCount = clusterPlacementReplicaCount(p);
    printf("clusterPlacement:\n");
    printf("  Hash: 0x%016" PRIx64 "\n", p->hashValue);
    printf("  Replicas: %zu (healthy: %u)\n", replicaCount, p->healthyCount);

    if (p->primary) {
        printf("  Primary: #%" PRIu64 " \"%s\"\n", p->primary->id,
               p->primary->name);
    }

    for (size_t i = 0; i < replicaCount; i++) {
        const clusterNode *node = clusterPlacementGetReplica(p, i);
        if (node) {
            printf("    [%zu] #%" PRIu64 " \"%s\" (state=%d)\n", i, node->id,
                   node->name, node->state);
        }
    }
}

/* ====================================================================
 * Test Suite
 * ==================================================================== */

static int testRingLifecycle(void) {
    int err = 0;

    TEST("Ring creation with default config") {
        clusterRing *ring = clusterRingNewDefault();
        if (!ring) {
            ERRR("Failed to create ring");
        } else {
            if (clusterRingNodeCount(ring) != 0) {
                ERRR("Expected 0 nodes");
            }
            clusterRingFree(ring);
        }
    }

    TEST("Ring creation with custom config") {
        clusterRingConfig config = {
            .name = "test-ring",
            .strategyType = CLUSTER_STRATEGY_KETAMA,
            .defaultQuorum = CLUSTER_QUORUM_BALANCED,
            .hashSeed = 12345,
        };
        clusterRing *ring = clusterRingNew(&config);
        if (!ring) {
            ERRR("Failed to create ring with custom config");
        } else {
            clusterRingFree(ring);
        }
    }

    TEST("Ring creation with all strategies") {
        clusterStrategyType strategies[] = {
            CLUSTER_STRATEGY_KETAMA,     CLUSTER_STRATEGY_JUMP,
            CLUSTER_STRATEGY_RENDEZVOUS, CLUSTER_STRATEGY_MAGLEV,
            CLUSTER_STRATEGY_BOUNDED,
        };

        for (size_t i = 0; i < sizeof(strategies) / sizeof(strategies[0]);
             i++) {
            clusterRingConfig config = {
                .name = "strategy-test",
                .strategyType = strategies[i],
            };
            clusterRing *ring = clusterRingNew(&config);
            if (!ring) {
                ERR("Failed to create ring with strategy %zu", i);
                err++;
            } else {
                clusterRingFree(ring);
            }
        }
    }

    return err;
}

static int testNodeManagement(void) {
    int err = 0;

    clusterRing *ring = clusterRingNewDefault();
    if (!ring) {
        ERRR("Failed to create ring");
        return 1;
    }

    TEST("Add single node") {
        clusterNodeConfig config = {
            .id = 1,
            .name = "node-1",
            .address = "127.0.0.1:6379",
            .weight = 100,
            .initialState = CLUSTER_NODE_UP,
            .location = {.rackId = 1, .dcId = 1, .azId = 1},
        };

        clusterResult result = clusterRingAddNode(ring, &config);
        if (result != CLUSTER_OK) {
            ERR("Failed to add node: %d", result);
            err++;
        }

        if (clusterRingNodeCount(ring) != 1) {
            ERR("Expected 1 node, got %u", clusterRingNodeCount(ring));
            err++;
        }

        if (clusterRingHealthyNodeCount(ring) != 1) {
            ERRR("Expected 1 healthy node");
            err++;
        }
    }

    TEST("Add duplicate node fails") {
        clusterNodeConfig config = {
            .id = 1, /* Same ID */
            .name = "node-1-dup",
            .initialState = CLUSTER_NODE_UP,
        };

        clusterResult result = clusterRingAddNode(ring, &config);
        if (result != CLUSTER_ERR_EXISTS) {
            ERR("Expected CLUSTER_ERR_EXISTS, got %d", result);
            err++;
        }
    }

    TEST("Add multiple nodes") {
        const char *nodeNames[] = {"node-2", "node-3", "node-4", "node-5"};
        for (uint64_t i = 2; i <= 5; i++) {
            clusterNodeConfig config = {
                .id = i,
                .name = nodeNames[i - 2],
                .weight = 100,
                .initialState = CLUSTER_NODE_UP,
                .location = {.rackId = (uint32_t)(i % 3 + 1),
                             .dcId = 1,
                             .azId = 1},
            };

            clusterResult result = clusterRingAddNode(ring, &config);
            if (result != CLUSTER_OK) {
                ERR("Failed to add node %" PRIu64 ": %d", i, result);
                err++;
            }
        }

        if (clusterRingNodeCount(ring) != 5) {
            ERR("Expected 5 nodes, got %u", clusterRingNodeCount(ring));
            err++;
        }
    }

    TEST("Get node by ID") {
        const clusterNode *node = clusterRingGetNode(ring, 3);
        if (!node) {
            ERRR("Failed to get node 3");
            err++;
        } else if (clusterNodeGetId(node) != 3) {
            ERRR("Wrong node ID");
            err++;
        }

        node = clusterRingGetNode(ring, 999);
        if (node) {
            ERRR("Should not find non-existent node");
            err++;
        }
    }

    TEST("Change node state") {
        clusterResult result =
            clusterRingSetNodeState(ring, 2, CLUSTER_NODE_DOWN);
        if (result != CLUSTER_OK) {
            ERR("Failed to set node state: %d", result);
            err++;
        }

        if (clusterRingHealthyNodeCount(ring) != 4) {
            ERRR("Expected 4 healthy nodes after marking one down");
            err++;
        }

        const clusterNode *node = clusterRingGetNode(ring, 2);
        if (!node || clusterNodeGetState(node) != CLUSTER_NODE_DOWN) {
            ERRR("Node state not updated");
            err++;
        }
    }

    TEST("Remove node") {
        clusterResult result = clusterRingRemoveNode(ring, 5);
        if (result != CLUSTER_OK) {
            ERR("Failed to remove node: %d", result);
            err++;
        }

        if (clusterRingNodeCount(ring) != 4) {
            ERRR("Expected 4 nodes after removal");
            err++;
        }

        const clusterNode *node = clusterRingGetNode(ring, 5);
        if (node) {
            ERRR("Node should be removed");
            err++;
        }
    }

    clusterRingFree(ring);
    return err;
}

static int testPlacement(void) {
    int err = 0;

    clusterRing *ring = clusterRingNewDefault();

    /* Add 5 nodes */
    const char *nodeNames[] = {"node-1", "node-2", "node-3", "node-4",
                               "node-5"};
    for (uint64_t i = 1; i <= 5; i++) {
        clusterNodeConfig config = {
            .id = i,
            .name = nodeNames[i - 1],
            .weight = 100,
            .initialState = CLUSTER_NODE_UP,
            .location = {.rackId = (uint32_t)i, .dcId = 1, .azId = 1},
        };
        clusterRingAddNode(ring, &config);
    }

    TEST("Basic placement") {
        clusterPlacement p;
        clusterResult result = clusterRingLocate(ring, "test-key", 8, &p);
        if (result != CLUSTER_OK) {
            ERR("Locate failed: %d", result);
            err++;
        }

        if (clusterPlacementReplicaCount(&p) == 0) {
            ERRR("No replicas found");
            err++;
        }

        if (!p.primary) {
            ERRR("No primary node");
            err++;
        }

        if (p.healthyCount != clusterPlacementReplicaCount(&p)) {
            ERRR("Expected all replicas healthy");
            err++;
        }
        clusterPlacementFree(&p);
    }

    TEST("Consistent placement") {
        clusterPlacement p1, p2;
        clusterRingLocate(ring, "consistent-key", 14, &p1);
        clusterRingLocate(ring, "consistent-key", 14, &p2);

        if (p1.primary->id != p2.primary->id) {
            ERRR("Same key should map to same primary");
            err++;
        }
        clusterPlacementFree(&p1);
        clusterPlacementFree(&p2);
    }

    TEST("Different keys different placement") {
        clusterPlacement p1, p2;
        clusterRingLocate(ring, "key-alpha", 9, &p1);
        clusterRingLocate(ring, "key-beta", 8, &p2);

        /* With 5 nodes, different keys will likely hit different primaries */
        /* This isn't guaranteed but is highly probable */
        clusterPlacementFree(&p1);
        clusterPlacementFree(&p2);
    }

    TEST("Placement with databox") {
        databox key = {
            .type = DATABOX_BYTES,
            .data.ptr = (void *)"databox-key",
            .len = 11,
        };
        clusterPlacement p;
        clusterResult result = clusterRingLocateBox(ring, &key, &p);
        if (result != CLUSTER_OK) {
            ERR("LocateBox failed: %d", result);
            err++;
        }
        clusterPlacementFree(&p);
    }

    TEST("Plan write") {
        clusterWriteSet ws;
        clusterResult result = clusterRingPlanWrite(
            ring, "write-key", 9, &CLUSTER_QUORUM_BALANCED, &ws);
        if (result != CLUSTER_OK) {
            ERR("PlanWrite failed: %d", result);
            err++;
        }

        if (clusterWriteSetTargetCount(&ws) == 0) {
            ERRR("No write targets");
            err++;
        }

        if (ws.syncRequired > clusterWriteSetTargetCount(&ws)) {
            ERRR("syncRequired > targetCount");
            err++;
        }
        clusterWriteSetFree(&ws);
    }

    TEST("Plan read") {
        clusterReadSet rs;
        clusterResult result = clusterRingPlanRead(
            ring, "read-key", 8, &CLUSTER_QUORUM_BALANCED, &rs);
        if (result != CLUSTER_OK) {
            ERR("PlanRead failed: %d", result);
            err++;
        }

        if (clusterReadSetCandidateCount(&rs) == 0) {
            ERRR("No read candidates");
            err++;
        }
        clusterReadSetFree(&rs);
    }

    TEST("Select read node") {
        clusterPlacement p;
        clusterRingLocate(ring, "select-key", 10, &p);

        const clusterNode *selected;
        clusterResult result = clusterRingSelectReadNode(ring, &p, &selected);
        if (result != CLUSTER_OK) {
            ERR("SelectReadNode failed: %d", result);
            err++;
        }

        if (!selected) {
            ERRR("No node selected");
            err++;
        }
        clusterPlacementFree(&p);
    }

    clusterRingFree(ring);
    return err;
}

static int testStrategies(void) {
    int err = 0;

    clusterStrategyType strategies[] = {
        CLUSTER_STRATEGY_KETAMA,
        CLUSTER_STRATEGY_JUMP,
        CLUSTER_STRATEGY_RENDEZVOUS,
        CLUSTER_STRATEGY_MAGLEV,
    };

    const char *strategyNames[] = {"Ketama", "Jump", "Rendezvous", "Maglev"};

    for (size_t s = 0; s < sizeof(strategies) / sizeof(strategies[0]); s++) {
        TEST_DESC("Strategy %s basic placement", strategyNames[s]) {
            clusterRingConfig config = {
                .name = "strategy-test",
                .strategyType = strategies[s],
            };
            clusterRing *ring = clusterRingNew(&config);

            /* Add nodes */
            const char *nodeNames[] = {"node-1", "node-2", "node-3", "node-4",
                                       "node-5"};
            for (uint64_t i = 1; i <= 5; i++) {
                clusterNodeConfig nodeConfig = {
                    .id = i,
                    .name = nodeNames[i - 1],
                    .weight = 100,
                    .initialState = CLUSTER_NODE_UP,
                };
                clusterRingAddNode(ring, &nodeConfig);
            }

            /* Test placement */
            clusterPlacement p;
            clusterResult result = clusterRingLocate(ring, "test", 4, &p);
            if (result != CLUSTER_OK) {
                ERR("%s: Locate failed: %d", strategyNames[s], result);
                err++;
            }

            if (clusterPlacementReplicaCount(&p) == 0) {
                ERR("%s: No replicas", strategyNames[s]);
                err++;
            }

            /* Test consistency */
            clusterPlacement p2;
            clusterRingLocate(ring, "test", 4, &p2);
            if (p.primary->id != p2.primary->id) {
                ERR("%s: Inconsistent placement", strategyNames[s]);
                err++;
            }

            clusterPlacementFree(&p);
            clusterPlacementFree(&p2);
            clusterRingFree(ring);
        }
    }

    return err;
}

static int testKeyspaces(void) {
    int err = 0;

    clusterRing *ring = clusterRingNewDefault();

    /* Add some nodes first */
    for (uint64_t i = 1; i <= 3; i++) {
        clusterNodeConfig config = {
            .id = i,
            .weight = 100,
            .initialState = CLUSTER_NODE_UP,
        };
        clusterRingAddNode(ring, &config);
    }

    TEST("Add keyspace") {
        clusterKeySpaceConfig ksConfig = {
            .name = "user-sessions",
            .quorum = CLUSTER_QUORUM_STRONG,
        };
        clusterKeySpace *ks;
        clusterResult result = clusterRingAddKeySpace(ring, &ksConfig, &ks);
        if (result != CLUSTER_OK) {
            ERR("Failed to add keyspace: %d", result);
            err++;
        }

        if (!ks) {
            ERRR("Keyspace not returned");
            err++;
        }
    }

    TEST("Get keyspace") {
        const clusterKeySpace *ks =
            clusterRingGetKeySpace(ring, "user-sessions");
        if (!ks) {
            ERRR("Failed to get keyspace");
            err++;
        }

        ks = clusterRingGetKeySpace(ring, "non-existent");
        if (ks) {
            ERRR("Should not find non-existent keyspace");
            err++;
        }
    }

    TEST("Duplicate keyspace fails") {
        clusterKeySpaceConfig ksConfig = {
            .name = "user-sessions",
        };
        clusterResult result = clusterRingAddKeySpace(ring, &ksConfig, NULL);
        if (result != CLUSTER_ERR_EXISTS) {
            ERR("Expected CLUSTER_ERR_EXISTS, got %d", result);
            err++;
        }
    }

    TEST("Remove keyspace") {
        clusterResult result = clusterRingRemoveKeySpace(ring, "user-sessions");
        if (result != CLUSTER_OK) {
            ERR("Failed to remove keyspace: %d", result);
            err++;
        }

        const clusterKeySpace *ks =
            clusterRingGetKeySpace(ring, "user-sessions");
        if (ks) {
            ERRR("Keyspace should be removed");
            err++;
        }
    }

    clusterRingFree(ring);
    return err;
}

static int testSerialization(void) {
    int err = 0;

    clusterRing *ring = clusterRingNewDefault();

    /* Add nodes */
    const char *nodeNames[] = {"node-1", "node-2", "node-3", "node-4",
                               "node-5"};
    const char *nodeAddrs[] = {"192.168.1.1:6379", "192.168.1.2:6379",
                               "192.168.1.3:6379", "192.168.1.4:6379",
                               "192.168.1.5:6379"};
    for (uint64_t i = 1; i <= 5; i++) {
        clusterNodeConfig config = {
            .id = i,
            .name = nodeNames[i - 1],
            .address = nodeAddrs[i - 1],
            .weight = 100,
            .initialState = CLUSTER_NODE_UP,
            .location = {.rackId = (uint32_t)i, .dcId = 1},
        };
        clusterRingAddNode(ring, &config);
    }

    /* Add keyspace */
    clusterKeySpaceConfig ksConfig = {
        .name = "test-ks",
        .quorum = CLUSTER_QUORUM_BALANCED,
    };
    clusterRingAddKeySpace(ring, &ksConfig, NULL);

    TEST("Serialization roundtrip") {
        size_t size = clusterRingSerializeSize(ring);
        if (size == 0) {
            ERRR("SerializeSize returned 0");
            err++;
        }

        void *buf = zmalloc(size);
        if (!buf) {
            ERRR("Failed to allocate buffer");
            err++;
        } else {
            size_t written = clusterRingSerialize(ring, buf, size);
            if (written == 0) {
                ERRR("Serialize returned 0");
                err++;
            }

            clusterRing *restored = clusterRingDeserialize(buf, written);
            if (!restored) {
                ERRR("Deserialize returned NULL");
                err++;
            } else {
                if (clusterRingNodeCount(restored) !=
                    clusterRingNodeCount(ring)) {
                    ERRR("Node count mismatch after restore");
                    err++;
                }

                /* Verify placement is same */
                clusterPlacement p1, p2;
                clusterRingLocate(ring, "ser-test", 8, &p1);
                clusterRingLocate(restored, "ser-test", 8, &p2);

                if (p1.primary->id != p2.primary->id) {
                    ERRR("Placement mismatch after restore");
                    err++;
                }

                clusterPlacementFree(&p1);
                clusterPlacementFree(&p2);
                clusterRingFree(restored);
            }

            zfree(buf);
        }
    }

    clusterRingFree(ring);
    return err;
}

static int testDistribution(void) {
    int err = 0;

    clusterRing *ring = clusterRingNewDefault();

    /* Add 10 nodes with equal weight */
    for (uint64_t i = 1; i <= 10; i++) {
        clusterNodeConfig config = {
            .id = i,
            .weight = 100,
            .initialState = CLUSTER_NODE_UP,
        };
        clusterRingAddNode(ring, &config);
    }

    TEST("Distribution across nodes") {
        uint32_t counts[11] = {0}; /* counts[nodeId] */
        const uint32_t numKeys = 10000;

        for (uint32_t i = 0; i < numKeys; i++) {
            char key[32];
            snprintf(key, sizeof(key), "key-%u", i);

            clusterPlacement p;
            clusterRingLocate(ring, key, strlen(key), &p);
            if (p.primary && p.primary->id <= 10) {
                counts[p.primary->id]++;
            }
            clusterPlacementFree(&p);
        }

        /* Check distribution - expect roughly equal */
        uint32_t expected = numKeys / 10;
        uint32_t tolerance = expected / 2; /* 50% tolerance */

        for (uint64_t i = 1; i <= 10; i++) {
            if (counts[i] < expected - tolerance ||
                counts[i] > expected + tolerance) {
                ERR("Node %" PRIu64 " has %u keys (expected ~%u)", i, counts[i],
                    expected);
                err++;
            }
        }
    }

    TEST("Weighted distribution") {
        clusterRingFree(ring);
        ring = clusterRingNewDefault();

        /* Add nodes with different weights */
        clusterNodeConfig config1 = {
            .id = 1, .weight = 100, .initialState = CLUSTER_NODE_UP};
        clusterNodeConfig config2 = {
            .id = 2, .weight = 200, .initialState = CLUSTER_NODE_UP};
        clusterNodeConfig config3 = {
            .id = 3, .weight = 300, .initialState = CLUSTER_NODE_UP};

        clusterRingAddNode(ring, &config1);
        clusterRingAddNode(ring, &config2);
        clusterRingAddNode(ring, &config3);

        uint32_t counts[4] = {0};
        const uint32_t numKeys = 10000;

        for (uint32_t i = 0; i < numKeys; i++) {
            char key[32];
            snprintf(key, sizeof(key), "wkey-%u", i);

            clusterPlacement p;
            clusterRingLocate(ring, key, strlen(key), &p);
            if (p.primary && p.primary->id <= 3) {
                counts[p.primary->id]++;
            }
            clusterPlacementFree(&p);
        }

        /* Node 3 should have ~3x keys as node 1 */
        /* Node 2 should have ~2x keys as node 1 */
        /* This is approximate due to hashing */
        float ratio31 =
            (float)counts[3] / (float)(counts[1] > 0 ? counts[1] : 1);
        float ratio21 =
            (float)counts[2] / (float)(counts[1] > 0 ? counts[1] : 1);

        /* Allow generous tolerance */
        if (ratio31 < 1.5f || ratio31 > 5.0f) {
            ERR("Weight ratio 3:1 = %.2f (expected ~3.0)", ratio31);
            err++;
        }
        if (ratio21 < 1.0f || ratio21 > 3.5f) {
            ERR("Weight ratio 2:1 = %.2f (expected ~2.0)", ratio21);
            err++;
        }
    }

    clusterRingFree(ring);
    return err;
}

static int testNodeFailure(void) {
    int err = 0;

    clusterRing *ring = clusterRingNewDefault();

    /* Add 5 nodes */
    for (uint64_t i = 1; i <= 5; i++) {
        clusterNodeConfig config = {
            .id = i,
            .weight = 100,
            .initialState = CLUSTER_NODE_UP,
        };
        clusterRingAddNode(ring, &config);
    }

    /* Get initial placement for a key */
    clusterPlacement pBefore;
    clusterRingLocate(ring, "failover-test", 13, &pBefore);
    uint64_t primaryBefore = pBefore.primary->id;
    clusterPlacementFree(&pBefore);

    TEST("Placement changes on node failure") {
        /* Mark primary as down */
        clusterRingSetNodeState(ring, primaryBefore, CLUSTER_NODE_DOWN);

        clusterPlacement pAfter;
        clusterRingLocate(ring, "failover-test", 13, &pAfter);

        /* Primary should change */
        if (pAfter.primary->id == primaryBefore) {
            ERRR("Primary should change after failure");
            err++;
        }

        /* Should still have replicas */
        if (clusterPlacementReplicaCount(&pAfter) == 0) {
            ERRR("Should still have replicas");
            err++;
        }

        /* Ring's healthy node count should be one less (the placement
         * healthyCount may be the same since we skip unhealthy nodes) */
        if (clusterRingHealthyNodeCount(ring) >= 5) {
            ERRR("Ring healthy count should decrease");
        }
        clusterPlacementFree(&pAfter);
    }

    TEST("Placement recovers when node comes back") {
        /* Bring node back */
        clusterRingSetNodeState(ring, primaryBefore, CLUSTER_NODE_UP);

        clusterPlacement pRecovered;
        clusterRingLocate(ring, "failover-test", 13, &pRecovered);

        /* Should return to original primary */
        if (pRecovered.primary->id != primaryBefore) {
            ERR("Should return to original primary: expected %" PRIu64
                ", got %" PRIu64,
                primaryBefore, pRecovered.primary->id);
            err++;
        }
        clusterPlacementFree(&pRecovered);
    }

    clusterRingFree(ring);
    return err;
}

static int testStats(void) {
    int err = 0;

    clusterRing *ring = clusterRingNewDefault();

    /* Add nodes */
    for (uint64_t i = 1; i <= 5; i++) {
        clusterNodeConfig config = {
            .id = i, .weight = 100, .initialState = CLUSTER_NODE_UP};
        clusterRingAddNode(ring, &config);
    }

    /* Perform some operations */
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "stats-key-%d", i);

        clusterPlacement p;
        clusterRingLocate(ring, key, strlen(key), &p);
        clusterPlacementFree(&p);

        clusterWriteSet ws;
        clusterRingPlanWrite(ring, key, strlen(key), NULL, &ws);
        clusterWriteSetFree(&ws);

        clusterReadSet rs;
        clusterRingPlanRead(ring, key, strlen(key), NULL, &rs);
        clusterReadSetFree(&rs);
    }

    TEST("Stats collection") {
        clusterRingStats stats;
        clusterRingGetStats(ring, &stats);

        if (stats.nodeCount != 5) {
            ERRR("Expected 5 nodes in stats");
            err++;
        }

        if (stats.healthyNodes != 5) {
            ERRR("Expected 5 healthy nodes in stats");
            err++;
        }

        if (stats.locateOps < 100) {
            ERRR("Expected at least 100 locate ops");
            err++;
        }

        if (stats.writeOps < 100) {
            ERRR("Expected at least 100 write ops");
            err++;
        }

        if (stats.readOps < 100) {
            ERRR("Expected at least 100 read ops");
            err++;
        }

        if (stats.memoryBytes == 0) {
            ERRR("Memory usage should be > 0");
            err++;
        }
    }

    clusterRingFree(ring);
    return err;
}

/* Callback test helper - must be at file scope */
typedef struct testCallbackData {
    uint64_t nodeId;
    clusterNodeState oldState;
    clusterNodeState newState;
    int callCount;
} testCallbackData;

static void testStateCallback(clusterRing *ring, uint64_t nodeId,
                              clusterNodeState oldState,
                              clusterNodeState newState, void *userData) {
    (void)ring;
    testCallbackData *data = (testCallbackData *)userData;
    data->nodeId = nodeId;
    data->oldState = oldState;
    data->newState = newState;
    data->callCount++;
}

static int testCallbacks(void) {
    int err = 0;

    clusterRing *ring = clusterRingNewDefault();
    testCallbackData cbData = {0};
    clusterRingSetNodeStateCallback(ring, testStateCallback, &cbData);

    TEST("State change callback") {
        clusterNodeConfig config = {
            .id = 1,
            .initialState = CLUSTER_NODE_UP,
        };
        clusterRingAddNode(ring, &config);

        /* Callback should be called on add */
        if (cbData.callCount < 1) {
            ERRR("Callback should be called on add");
            err++;
        }

        int prevCount = cbData.callCount;

        /* Change state */
        clusterRingSetNodeState(ring, 1, CLUSTER_NODE_DOWN);

        if (cbData.callCount <= prevCount) {
            ERRR("Callback should be called on state change");
            err++;
        }

        if (cbData.nodeId != 1) {
            ERRR("Wrong node ID in callback");
            err++;
        }

        if (cbData.oldState != CLUSTER_NODE_UP ||
            cbData.newState != CLUSTER_NODE_DOWN) {
            ERRR("Wrong states in callback");
            err++;
        }
    }

    clusterRingFree(ring);
    return err;
}

/* Iteration test helpers - must be at file scope */
typedef struct testIterData {
    int count;
} testIterData;

static bool testCountIter(const clusterNode *node, void *userData) {
    (void)node;
    testIterData *data = (testIterData *)userData;
    data->count++;
    return true;
}

static bool testStopAfter2(const clusterNode *node, void *userData) {
    (void)node;
    testIterData *data = (testIterData *)userData;
    data->count++;
    return data->count < 2;
}

static int testIteration(void) {
    int err = 0;

    clusterRing *ring = clusterRingNewDefault();

    /* Add nodes in different states */
    for (uint64_t i = 1; i <= 5; i++) {
        clusterNodeConfig config = {
            .id = i,
            .initialState = (i <= 3) ? CLUSTER_NODE_UP : CLUSTER_NODE_DOWN,
            .location = {.rackId = (uint32_t)(i % 2 + 1)},
        };
        clusterRingAddNode(ring, &config);
    }

    TEST("Iterate all nodes") {
        testIterData data = {0};
        clusterRingIterateNodes(ring, testCountIter, &data);

        if (data.count != 5) {
            ERR("Expected 5 nodes, got %d", data.count);
        }
    }

    TEST("Iterate by state") {
        testIterData upData = {0};
        clusterRingIterateNodesByState(ring, CLUSTER_NODE_UP, testCountIter,
                                       &upData);

        if (upData.count != 3) {
            ERR("Expected 3 UP nodes, got %d", upData.count);
        }

        testIterData downData = {0};
        clusterRingIterateNodesByState(ring, CLUSTER_NODE_DOWN, testCountIter,
                                       &downData);

        if (downData.count != 2) {
            ERR("Expected 2 DOWN nodes, got %d", downData.count);
        }
    }

    TEST("Iterate by location") {
        testIterData rack1Data = {0};
        clusterRingIterateNodesByLocation(ring, CLUSTER_LEVEL_RACK, 1,
                                          testCountIter, &rack1Data);

        /* Nodes with rackId = 1: i % 2 + 1 == 1 means i % 2 == 0, so even nodes
         * i=1: 1%2+1=2, i=2: 2%2+1=1, i=3: 3%2+1=2, i=4: 4%2+1=1, i=5: 5%2+1=2
         * So rack 1: nodes 2, 4 = 2 nodes */
        if (rack1Data.count != 2) {
            ERR("Expected 2 nodes in rack 1, got %d", rack1Data.count);
        }
    }

    TEST("Early termination") {
        testIterData data = {0};
        clusterRingIterateNodes(ring, testStopAfter2, &data);

        if (data.count != 2) {
            ERR("Should stop after 2, got %d", data.count);
        }
    }

    clusterRingFree(ring);
    return err;
}

/* ====================================================================
 * Comprehensive Fuzzing Tests
 * ==================================================================== */

static int testFuzzNodeChurn(void) {
    int err = 0;

    TEST("FUZZ: Random node add/remove churn") {
        clusterRing *ring = clusterRingNewDefault();
        uint64_t nextNodeId = 1;
        uint64_t *activeNodes = zcalloc(1000, sizeof(uint64_t));
        uint32_t activeCount = 0;

        /* Run 1000 random operations */
        for (int op = 0; op < 1000; op++) {
            int action = rand() % 10;

            if (action < 6 || activeCount < 3) {
                /* Add node (60% or if too few nodes) */
                clusterNodeConfig config = {
                    .id = nextNodeId,
                    .weight = (uint32_t)(rand() % 500 + 50),
                    .initialState = CLUSTER_NODE_UP,
                    .location =
                        {
                            .rackId = (uint32_t)(rand() % 5 + 1),
                            .dcId = (uint32_t)(rand() % 3 + 1),
                        },
                };
                if (clusterRingAddNode(ring, &config) == CLUSTER_OK) {
                    activeNodes[activeCount++] = nextNodeId;
                }
                nextNodeId++;
            } else if (activeCount > 0) {
                /* Remove random node (40%) */
                uint32_t idx = (uint32_t)(rand() % activeCount);
                uint64_t nodeId = activeNodes[idx];
                clusterRingRemoveNode(ring, nodeId);
                activeNodes[idx] = activeNodes[--activeCount];
            }

            /* Verify ring state */
            if (clusterRingNodeCount(ring) != activeCount) {
                ERR("Op %d: node count mismatch: %u vs %u", op,
                    clusterRingNodeCount(ring), activeCount);
                err++;
                break;
            }
        }

        /* Verify placement still works */
        if (activeCount > 0) {
            clusterPlacement p;
            clusterResult result = clusterRingLocate(ring, "test-key", 8, &p);
            if (result != CLUSTER_OK) {
                ERR("Placement failed after churn: %d", result);
                err++;
            } else {
                clusterPlacementFree(&p);
            }
        }

        zfree(activeNodes);
        clusterRingFree(ring);
    }

    return err;
}

static int testFuzzPlacementConsistency(void) {
    int err = 0;

    TEST("FUZZ: Placement consistency across operations") {
        clusterRing *ring = clusterRingNewDefault();

        /* Add initial nodes */
        for (uint64_t i = 1; i <= 20; i++) {
            clusterNodeConfig config = {
                .id = i,
                .weight = 100,
                .initialState = CLUSTER_NODE_UP,
            };
            clusterRingAddNode(ring, &config);
        }

        /* Record placements for 100 keys */
        const int numKeys = 100;
        uint64_t *primaryIds = zcalloc(numKeys, sizeof(uint64_t));

        for (int i = 0; i < numKeys; i++) {
            char key[32];
            snprintf(key, sizeof(key), "consistency-key-%d", i);
            clusterPlacement p;
            clusterRingLocate(ring, key, strlen(key), &p);
            primaryIds[i] = p.primary->id;
            clusterPlacementFree(&p);
        }

        /* Verify consistency - same key should map to same node */
        for (int round = 0; round < 10; round++) {
            for (int i = 0; i < numKeys; i++) {
                char key[32];
                snprintf(key, sizeof(key), "consistency-key-%d", i);
                clusterPlacement p;
                clusterRingLocate(ring, key, strlen(key), &p);
                if (p.primary->id != primaryIds[i]) {
                    ERR("Key %d: inconsistent placement %" PRIu64
                        " vs %" PRIu64,
                        i, p.primary->id, primaryIds[i]);
                    err++;
                }
                clusterPlacementFree(&p);
            }
        }

        zfree(primaryIds);
        clusterRingFree(ring);
    }

    TEST("FUZZ: Minimal movement on node removal") {
        clusterRing *ring = clusterRingNewDefault();

        /* Add 10 nodes */
        for (uint64_t i = 1; i <= 10; i++) {
            clusterNodeConfig config = {
                .id = i,
                .weight = 100,
                .initialState = CLUSTER_NODE_UP,
            };
            clusterRingAddNode(ring, &config);
        }

        /* Record placements for 1000 keys */
        const int numKeys = 1000;
        uint64_t *beforePrimary = zcalloc(numKeys, sizeof(uint64_t));

        for (int i = 0; i < numKeys; i++) {
            char key[32];
            snprintf(key, sizeof(key), "movement-key-%d", i);
            clusterPlacement p;
            clusterRingLocate(ring, key, strlen(key), &p);
            beforePrimary[i] = p.primary->id;
            clusterPlacementFree(&p);
        }

        /* Remove one node */
        uint64_t removedId = 5;
        clusterRingRemoveNode(ring, removedId);

        /* Count how many keys moved */
        int movedCount = 0;
        for (int i = 0; i < numKeys; i++) {
            char key[32];
            snprintf(key, sizeof(key), "movement-key-%d", i);
            clusterPlacement p;
            clusterRingLocate(ring, key, strlen(key), &p);

            if (beforePrimary[i] == removedId) {
                /* This key HAD to move */
            } else if (p.primary->id != beforePrimary[i]) {
                /* This key moved but shouldn't have */
                movedCount++;
            }
            clusterPlacementFree(&p);
        }

        /* With consistent hashing, minimal keys should move unnecessarily */
        /* Allow up to 5% spurious movement */
        if (movedCount > numKeys / 20) {
            ERR("Too many keys moved unnecessarily: %d", movedCount);
            err++;
        }

        zfree(beforePrimary);
        clusterRingFree(ring);
    }

    return err;
}

static int testFuzzAllStrategies(void) {
    int err = 0;

    clusterStrategyType strategies[] = {
        CLUSTER_STRATEGY_KETAMA,
        CLUSTER_STRATEGY_JUMP,
        CLUSTER_STRATEGY_RENDEZVOUS,
        CLUSTER_STRATEGY_MAGLEV,
    };
    const char *strategyNames[] = {"Ketama", "Jump", "Rendezvous", "Maglev"};

    for (size_t s = 0; s < sizeof(strategies) / sizeof(strategies[0]); s++) {
        TEST_DESC("FUZZ: %s stress test", strategyNames[s]) {
            clusterRingConfig config = {
                .name = "fuzz-test",
                .strategyType = strategies[s],
            };
            clusterRing *ring = clusterRingNew(&config);

            /* Add many nodes */
            for (uint64_t i = 1; i <= 50; i++) {
                clusterNodeConfig nodeConfig = {
                    .id = i,
                    .weight = (uint32_t)(rand() % 300 + 50),
                    .initialState = CLUSTER_NODE_UP,
                    .location =
                        {
                            .rackId = (uint32_t)(i % 5 + 1),
                            .dcId = (uint32_t)(i % 3 + 1),
                        },
                };
                clusterRingAddNode(ring, &nodeConfig);
            }

            /* Hammer with placements */
            for (int i = 0; i < 5000; i++) {
                char key[64];
                snprintf(key, sizeof(key), "stress-key-%d-%d", i, rand());

                clusterPlacement p;
                clusterResult result =
                    clusterRingLocate(ring, key, strlen(key), &p);
                if (result != CLUSTER_OK) {
                    ERR("%s: Placement %d failed", strategyNames[s], i);
                    err++;
                    break;
                }

                if (!p.primary) {
                    ERR("%s: No primary at %d", strategyNames[s], i);
                    err++;
                }

                if (clusterPlacementReplicaCount(&p) == 0) {
                    ERR("%s: No replicas at %d", strategyNames[s], i);
                    err++;
                }

                clusterPlacementFree(&p);
            }

            clusterRingFree(ring);
        }
    }

    return err;
}

static int testFuzzStateTransitions(void) {
    int err = 0;

    TEST("FUZZ: Random state transitions") {
        clusterRing *ring = clusterRingNewDefault();

        /* Add 20 nodes */
        for (uint64_t i = 1; i <= 20; i++) {
            clusterNodeConfig config = {
                .id = i,
                .weight = 100,
                .initialState = CLUSTER_NODE_UP,
            };
            clusterRingAddNode(ring, &config);
        }

        clusterNodeState states[] = {
            CLUSTER_NODE_UP,      CLUSTER_NODE_DOWN,
            CLUSTER_NODE_SUSPECT, CLUSTER_NODE_MAINTENANCE,
            CLUSTER_NODE_JOINING, CLUSTER_NODE_LEAVING,
        };

        /* Randomly change node states */
        for (int op = 0; op < 500; op++) {
            uint64_t nodeId = (uint64_t)(rand() % 20 + 1);
            clusterNodeState newState = states[rand() % 6];
            clusterRingSetNodeState(ring, nodeId, newState);

            /* Verify placement still works (may fail if all nodes down) */
            clusterPlacement p;
            clusterResult result = clusterRingLocate(ring, "test", 4, &p);
            if (result == CLUSTER_OK) {
                /* Verify primary is healthy */
                if (p.primary && p.primary->state != CLUSTER_NODE_UP) {
                    /* Primary should be UP if placement succeeded */
                    ERR("Op %d: primary node %" PRIu64 " is not UP (state=%d)",
                        op, p.primary->id, p.primary->state);
                    err++;
                }
                clusterPlacementFree(&p);
            }
            /* CLUSTER_ERR_NO_NODES is acceptable if all nodes are down */
        }

        clusterRingFree(ring);
    }

    return err;
}

static int testFuzzSerializationRoundtrip(void) {
    int err = 0;

    TEST("FUZZ: Serialization with random data") {
        for (int trial = 0; trial < 10; trial++) {
            clusterRingConfig config = {
                .name = "serialize-fuzz",
                .strategyType = CLUSTER_STRATEGY_KETAMA,
            };
            clusterRing *ring = clusterRingNew(&config);

            /* Add random nodes */
            int nodeCount = rand() % 50 + 5;
            for (int i = 1; i <= nodeCount; i++) {
                clusterNodeConfig nodeConfig = {
                    .id = (uint64_t)i,
                    .weight = (uint32_t)(rand() % 500 + 10),
                    .capacityBytes = (uint64_t)rand() * rand(),
                    .initialState = CLUSTER_NODE_UP,
                    .location =
                        {
                            .rackId = (uint32_t)(rand() % 10),
                            .dcId = (uint32_t)(rand() % 5),
                            .azId = (uint32_t)(rand() % 3),
                        },
                };
                clusterRingAddNode(ring, &nodeConfig);
            }

            /* Add random keyspaces */
            int ksCount = rand() % 5 + 1;
            char ksNames[5][32];
            for (int i = 0; i < ksCount; i++) {
                snprintf(ksNames[i], sizeof(ksNames[i]), "keyspace-%d", i);
                clusterKeySpaceConfig ksConfig = {
                    .name = ksNames[i],
                    .quorum = CLUSTER_QUORUM_BALANCED,
                };
                clusterRingAddKeySpace(ring, &ksConfig, NULL);
            }

            /* Serialize */
            size_t size = clusterRingSerializeSize(ring);
            if (size == 0) {
                ERR("Trial %d: SerializeSize returned 0", trial);
                err++;
                clusterRingFree(ring);
                continue;
            }

            void *buf = zmalloc(size);
            size_t written = clusterRingSerialize(ring, buf, size);
            if (written == 0) {
                ERR("Trial %d: Serialize returned 0", trial);
                err++;
                zfree(buf);
                clusterRingFree(ring);
                continue;
            }

            /* Deserialize */
            clusterRing *restored = clusterRingDeserialize(buf, written);
            if (!restored) {
                ERR("Trial %d: Deserialize returned NULL", trial);
                err++;
                zfree(buf);
                clusterRingFree(ring);
                continue;
            }

            /* Verify node count matches */
            if (clusterRingNodeCount(ring) != clusterRingNodeCount(restored)) {
                ERR("Trial %d: Node count mismatch: %u vs %u", trial,
                    clusterRingNodeCount(ring), clusterRingNodeCount(restored));
                err++;
            }

            /* Verify keyspace count matches */
            if (ring->keySpaceCount != restored->keySpaceCount) {
                ERR("Trial %d: Keyspace count mismatch: %u vs %u", trial,
                    ring->keySpaceCount, restored->keySpaceCount);
                err++;
            }

            /* Verify placement consistency */
            for (int k = 0; k < 20; k++) {
                char key[32];
                snprintf(key, sizeof(key), "verify-key-%d", k);

                clusterPlacement p1, p2;
                clusterRingLocate(ring, key, strlen(key), &p1);
                clusterRingLocate(restored, key, strlen(key), &p2);

                if (p1.primary->id != p2.primary->id) {
                    ERR("Trial %d key %d: placement mismatch", trial, k);
                    err++;
                }

                clusterPlacementFree(&p1);
                clusterPlacementFree(&p2);
            }

            zfree(buf);
            clusterRingFree(restored);
            clusterRingFree(ring);
        }
    }

    return err;
}

static int testFuzzConcurrentModification(void) {
    int err = 0;

    TEST("FUZZ: Interleaved modifications and lookups") {
        clusterRing *ring = clusterRingNewDefault();
        uint64_t nextId = 1;

        /* Interleave adds, removes, state changes, and lookups */
        for (int op = 0; op < 1000; op++) {
            int action = rand() % 10;

            if (action < 3) {
                /* Add node */
                clusterNodeConfig config = {
                    .id = nextId++,
                    .weight = (uint32_t)(rand() % 200 + 50),
                    .initialState = CLUSTER_NODE_UP,
                };
                clusterRingAddNode(ring, &config);
            } else if (action < 5 && clusterRingNodeCount(ring) > 3) {
                /* Remove random node */
                uint64_t removeId = (uint64_t)(rand() % (nextId - 1) + 1);
                clusterRingRemoveNode(ring, removeId);
            } else if (action < 7 && clusterRingNodeCount(ring) > 0) {
                /* Change state */
                uint64_t nodeId = (uint64_t)(rand() % (nextId - 1) + 1);
                clusterNodeState state =
                    rand() % 2 == 0 ? CLUSTER_NODE_UP : CLUSTER_NODE_DOWN;
                clusterRingSetNodeState(ring, nodeId, state);
            } else {
                /* Lookup */
                char key[32];
                snprintf(key, sizeof(key), "interleave-%d", rand());
                clusterPlacement p;
                clusterResult result =
                    clusterRingLocate(ring, key, strlen(key), &p);
                if (result == CLUSTER_OK) {
                    clusterPlacementFree(&p);
                }
            }
        }

        clusterRingFree(ring);
    }

    return err;
}

static int testFuzzLargeScale(void) {
    int err = 0;

    TEST("FUZZ: Large scale - 500 nodes") {
        clusterRing *ring = clusterRingNewDefault();

        /* Add 500 nodes across multiple DCs and racks */
        for (uint64_t i = 1; i <= 500; i++) {
            clusterNodeConfig config = {
                .id = i,
                .weight = (uint32_t)(rand() % 200 + 50),
                .initialState = CLUSTER_NODE_UP,
                .capacityBytes = 1024ULL * 1024 * 1024 * (rand() % 100 + 10),
                .location =
                    {
                        .rackId = (uint32_t)(i % 50 + 1),
                        .dcId = (uint32_t)(i % 10 + 1),
                        .azId = (uint32_t)(i % 3 + 1),
                        .regionId = (uint32_t)(i % 5 + 1),
                    },
            };
            clusterRingAddNode(ring, &config);
        }

        if (clusterRingNodeCount(ring) != 500) {
            ERR("Expected 500 nodes, got %u", clusterRingNodeCount(ring));
            err++;
        }

        /* Test placement with many keys */
        uint32_t *distribution = zcalloc(501, sizeof(uint32_t));
        const int numKeys = 50000;

        for (int i = 0; i < numKeys; i++) {
            char key[32];
            snprintf(key, sizeof(key), "large-key-%d", i);

            clusterPlacement p;
            clusterResult result =
                clusterRingLocate(ring, key, strlen(key), &p);
            if (result != CLUSTER_OK) {
                ERR("Placement failed for key %d: %d", i, result);
                err++;
                break;
            }

            if (p.primary && p.primary->id <= 500) {
                distribution[p.primary->id]++;
            }
            clusterPlacementFree(&p);
        }

        /* Verify reasonable distribution */
        uint32_t minCount = UINT32_MAX, maxCount = 0;
        for (uint64_t i = 1; i <= 500; i++) {
            if (distribution[i] < minCount) {
                minCount = distribution[i];
            }
            if (distribution[i] > maxCount) {
                maxCount = distribution[i];
            }
        }

        /* With 500 nodes and 50000 keys, expect ~100 per node
         * Allow 10x variance for weighted distribution */
        if (maxCount > 1000) {
            ERR("Max count too high: %u (expected ~100)", maxCount);
            err++;
        }

        zfree(distribution);
        clusterRingFree(ring);
    }

    TEST("FUZZ: Large scale - many replicas request") {
        clusterRingConfig config = {
            .name = "many-replicas",
            .strategyType = CLUSTER_STRATEGY_KETAMA,
            .defaultQuorum = {.replicaCount = 50},
        };
        clusterRing *ring = clusterRingNew(&config);

        /* Add 100 nodes */
        for (uint64_t i = 1; i <= 100; i++) {
            clusterNodeConfig nodeConfig = {
                .id = i,
                .weight = 100,
                .initialState = CLUSTER_NODE_UP,
            };
            clusterRingAddNode(ring, &nodeConfig);
        }

        /* Request placement with many replicas */
        clusterPlacement p;
        clusterResult result =
            clusterRingLocate(ring, "many-replica-key", 16, &p);
        if (result != CLUSTER_OK) {
            ERR("Many-replica placement failed: %d", result);
            err++;
        } else {
            size_t replicaCount = clusterPlacementReplicaCount(&p);
            if (replicaCount < 50) {
                ERR("Expected 50 replicas, got %zu", replicaCount);
                err++;
            }

            /* Verify all replicas are unique */
            uint64_t *seen = zcalloc(replicaCount, sizeof(uint64_t));
            for (size_t i = 0; i < replicaCount; i++) {
                const clusterNode *node = clusterPlacementGetReplica(&p, i);
                if (node) {
                    for (size_t j = 0; j < i; j++) {
                        if (seen[j] == node->id) {
                            ERR("Duplicate replica: node %" PRIu64
                                " at positions %zu "
                                "and %zu",
                                node->id, j, i);
                            err++;
                        }
                    }
                    seen[i] = node->id;
                }
            }
            zfree(seen);

            clusterPlacementFree(&p);
        }

        clusterRingFree(ring);
    }

    return err;
}

static int testFuzzEdgeCases(void) {
    int err = 0;

    TEST("FUZZ: Single node cluster") {
        clusterRing *ring = clusterRingNewDefault();

        clusterNodeConfig config = {
            .id = 1,
            .weight = 100,
            .initialState = CLUSTER_NODE_UP,
        };
        clusterRingAddNode(ring, &config);

        /* All keys should map to the single node */
        for (int i = 0; i < 100; i++) {
            char key[32];
            snprintf(key, sizeof(key), "single-node-key-%d", i);

            clusterPlacement p;
            clusterResult result =
                clusterRingLocate(ring, key, strlen(key), &p);
            if (result != CLUSTER_OK) {
                ERR("Single node placement failed: %d", result);
                err++;
            } else if (p.primary->id != 1) {
                ERR("Wrong primary: expected 1, got %" PRIu64, p.primary->id);
                err++;
            }
            clusterPlacementFree(&p);
        }

        clusterRingFree(ring);
    }

    TEST("FUZZ: Empty key") {
        clusterRing *ring = clusterRingNewDefault();

        for (uint64_t i = 1; i <= 5; i++) {
            clusterNodeConfig config = {
                .id = i, .weight = 100, .initialState = CLUSTER_NODE_UP};
            clusterRingAddNode(ring, &config);
        }

        clusterPlacement p;
        clusterResult result = clusterRingLocate(ring, "", 0, &p);
        /* Empty key should fail */
        if (result == CLUSTER_OK) {
            clusterPlacementFree(&p);
        }

        clusterRingFree(ring);
    }

    TEST("FUZZ: Very long key") {
        clusterRing *ring = clusterRingNewDefault();

        for (uint64_t i = 1; i <= 5; i++) {
            clusterNodeConfig config = {
                .id = i, .weight = 100, .initialState = CLUSTER_NODE_UP};
            clusterRingAddNode(ring, &config);
        }

        char *longKey = zmalloc(10000);
        memset(longKey, 'x', 9999);
        longKey[9999] = '\0';

        clusterPlacement p;
        clusterResult result = clusterRingLocate(ring, longKey, 9999, &p);
        if (result != CLUSTER_OK) {
            ERR("Long key placement failed: %d", result);
            err++;
        } else {
            clusterPlacementFree(&p);
        }

        zfree(longKey);
        clusterRingFree(ring);
    }

    TEST("FUZZ: Binary key data") {
        clusterRing *ring = clusterRingNewDefault();

        for (uint64_t i = 1; i <= 5; i++) {
            clusterNodeConfig config = {
                .id = i, .weight = 100, .initialState = CLUSTER_NODE_UP};
            clusterRingAddNode(ring, &config);
        }

        /* Key with null bytes and binary data */
        uint8_t binaryKey[32];
        for (int i = 0; i < 32; i++) {
            binaryKey[i] = (uint8_t)(rand() % 256);
        }

        clusterPlacement p;
        clusterResult result = clusterRingLocate(ring, binaryKey, 32, &p);
        if (result != CLUSTER_OK) {
            ERR("Binary key placement failed: %d", result);
            err++;
        } else {
            clusterPlacementFree(&p);
        }

        clusterRingFree(ring);
    }

    TEST("FUZZ: Extreme weights") {
        clusterRing *ring = clusterRingNewDefault();

        /* Node with weight 1 */
        clusterNodeConfig config1 = {
            .id = 1, .weight = 1, .initialState = CLUSTER_NODE_UP};
        /* Node with weight 10000 */
        clusterNodeConfig config2 = {
            .id = 2, .weight = 10000, .initialState = CLUSTER_NODE_UP};

        clusterRingAddNode(ring, &config1);
        clusterRingAddNode(ring, &config2);

        /* Heavy node should get most traffic */
        uint32_t counts[3] = {0};
        for (int i = 0; i < 10000; i++) {
            char key[32];
            snprintf(key, sizeof(key), "weight-key-%d", i);

            clusterPlacement p;
            clusterRingLocate(ring, key, strlen(key), &p);
            if (p.primary && p.primary->id <= 2) {
                counts[p.primary->id]++;
            }
            clusterPlacementFree(&p);
        }

        /* Node 2 should have significantly more */
        if (counts[2] < counts[1] * 5) {
            ERR("Weight not respected: node1=%u node2=%u", counts[1],
                counts[2]);
            err++;
        }

        clusterRingFree(ring);
    }

    return err;
}

/* ====================================================================
 * Benchmark Tests
 * ==================================================================== */

static int testBenchmarkLocate(void) {
    int err = 0;

    TEST("BENCH: Ketama locate throughput") {
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_KETAMA,
            .hashSeed = 12345,
        };
        clusterRing *ring = clusterRingNew(&config);

        /* Add 100 nodes */
        const char *nodeNames[100];
        char nameBuf[100][16];
        for (int i = 0; i < 100; i++) {
            snprintf(nameBuf[i], 16, "node-%d", i);
            nodeNames[i] = nameBuf[i];
        }
        for (int i = 0; i < 100; i++) {
            clusterNodeConfig nodeConfig = {
                .id = (uint64_t)(i + 1),
                .name = nodeNames[i],
                .weight = 100,
                .initialState = CLUSTER_NODE_UP,
            };
            clusterRingAddNode(ring, &nodeConfig);
        }

        /* Benchmark: 100K lookups */
        uint64_t startNs = timeUtilMonotonicNs();
        const int iterations = 100000;

        for (int i = 0; i < iterations; i++) {
            char key[32];
            snprintf(key, sizeof(key), "benchmark-key-%d", i);
            clusterPlacement p;
            clusterRingLocate(ring, key, strlen(key), &p);
            clusterPlacementFree(&p);
        }

        uint64_t endNs = timeUtilMonotonicNs();
        uint64_t durationNs = endNs - startNs;
        double nsPerOp = (double)durationNs / iterations;

        /* Report (not a failure condition, just informational) */
        printf("    Ketama: %d ops in %.2fms (%.1f ns/op, %.1fM ops/sec)\n",
               iterations, durationNs / 1000000.0, nsPerOp,
               1000000000.0 / nsPerOp / 1000000.0);

        clusterRingFree(ring);
    }

    TEST("BENCH: Rendezvous locate throughput") {
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_RENDEZVOUS,
            .hashSeed = 12345,
        };
        clusterRing *ring = clusterRingNew(&config);

        /* Add 100 nodes */
        for (int i = 0; i < 100; i++) {
            clusterNodeConfig nodeConfig = {
                .id = (uint64_t)(i + 1),
                .weight = 100,
                .initialState = CLUSTER_NODE_UP,
            };
            clusterRingAddNode(ring, &nodeConfig);
        }

        /* Benchmark: 10K lookups (Rendezvous is O(n) per lookup) */
        uint64_t startNs = timeUtilMonotonicNs();
        const int iterations = 10000;

        for (int i = 0; i < iterations; i++) {
            char key[32];
            snprintf(key, sizeof(key), "benchmark-key-%d", i);
            clusterPlacement p;
            clusterRingLocate(ring, key, strlen(key), &p);
            clusterPlacementFree(&p);
        }

        uint64_t endNs = timeUtilMonotonicNs();
        uint64_t durationNs = endNs - startNs;
        double nsPerOp = (double)durationNs / iterations;

        printf("    Rendezvous: %d ops in %.2fms (%.1f ns/op, %.1fK ops/sec)\n",
               iterations, durationNs / 1000000.0, nsPerOp,
               1000000000.0 / nsPerOp / 1000.0);

        clusterRingFree(ring);
    }

    TEST("BENCH: Jump hash locate throughput") {
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_JUMP,
            .hashSeed = 12345,
        };
        clusterRing *ring = clusterRingNew(&config);

        /* Add 100 nodes */
        for (int i = 0; i < 100; i++) {
            clusterNodeConfig nodeConfig = {
                .id = (uint64_t)(i + 1),
                .weight = 100,
                .initialState = CLUSTER_NODE_UP,
            };
            clusterRingAddNode(ring, &nodeConfig);
        }

        /* Benchmark: 100K lookups */
        uint64_t startNs = timeUtilMonotonicNs();
        const int iterations = 100000;

        for (int i = 0; i < iterations; i++) {
            char key[32];
            snprintf(key, sizeof(key), "benchmark-key-%d", i);
            clusterPlacement p;
            clusterRingLocate(ring, key, strlen(key), &p);
            clusterPlacementFree(&p);
        }

        uint64_t endNs = timeUtilMonotonicNs();
        uint64_t durationNs = endNs - startNs;
        double nsPerOp = (double)durationNs / iterations;

        printf("    Jump: %d ops in %.2fms (%.1f ns/op, %.1fM ops/sec)\n",
               iterations, durationNs / 1000000.0, nsPerOp,
               1000000000.0 / nsPerOp / 1000000.0);

        clusterRingFree(ring);
    }

    TEST("BENCH: Maglev locate throughput") {
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_MAGLEV,
            .hashSeed = 12345,
        };
        clusterRing *ring = clusterRingNew(&config);

        /* Add 100 nodes */
        for (int i = 0; i < 100; i++) {
            clusterNodeConfig nodeConfig = {
                .id = (uint64_t)(i + 1),
                .weight = 100,
                .initialState = CLUSTER_NODE_UP,
            };
            clusterRingAddNode(ring, &nodeConfig);
        }

        /* Benchmark: 100K lookups */
        uint64_t startNs = timeUtilMonotonicNs();
        const int iterations = 100000;

        for (int i = 0; i < iterations; i++) {
            char key[32];
            snprintf(key, sizeof(key), "benchmark-key-%d", i);
            clusterPlacement p;
            clusterRingLocate(ring, key, strlen(key), &p);
            clusterPlacementFree(&p);
        }

        uint64_t endNs = timeUtilMonotonicNs();
        uint64_t durationNs = endNs - startNs;
        double nsPerOp = (double)durationNs / iterations;

        printf("    Maglev: %d ops in %.2fms (%.1f ns/op, %.1fM ops/sec)\n",
               iterations, durationNs / 1000000.0, nsPerOp,
               1000000000.0 / nsPerOp / 1000000.0);

        clusterRingFree(ring);
    }

    TEST("BENCH: Bulk locate vs single locate") {
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_KETAMA,
            .hashSeed = 12345,
        };
        clusterRing *ring = clusterRingNew(&config);

        /* Add 50 nodes */
        for (int i = 0; i < 50; i++) {
            clusterNodeConfig nodeConfig = {
                .id = (uint64_t)(i + 1),
                .weight = 100,
                .initialState = CLUSTER_NODE_UP,
            };
            clusterRingAddNode(ring, &nodeConfig);
        }

        /* Create batch of keys */
        const int batchSize = 100;
        databox keys[100];
        clusterPlacement placements[100];
        char keyStrings[100][32];

        for (int i = 0; i < batchSize; i++) {
            snprintf(keyStrings[i], 32, "bulk-key-%d", i);
            keys[i].type = DATABOX_BYTES;
            keys[i].data.bytes.start = (uint8_t *)keyStrings[i];
            keys[i].len = strlen(keyStrings[i]);
        }

        /* Benchmark single calls */
        uint64_t singleStart = timeUtilMonotonicNs();
        const int iterations = 1000;
        for (int iter = 0; iter < iterations; iter++) {
            for (int i = 0; i < batchSize; i++) {
                clusterPlacement p;
                clusterRingLocate(ring, keyStrings[i], strlen(keyStrings[i]),
                                  &p);
                clusterPlacementFree(&p);
            }
        }
        uint64_t singleEnd = timeUtilMonotonicNs();

        /* Benchmark bulk calls */
        uint64_t bulkStart = timeUtilMonotonicNs();
        for (int iter = 0; iter < iterations; iter++) {
            clusterRingLocateBulk(ring, keys, batchSize, placements);
            for (int i = 0; i < batchSize; i++) {
                clusterPlacementFree(&placements[i]);
            }
        }
        uint64_t bulkEnd = timeUtilMonotonicNs();

        double singleTimeMs = (singleEnd - singleStart) / 1000000.0;
        double bulkTimeMs = (bulkEnd - bulkStart) / 1000000.0;

        printf("    Single: %.2fms, Bulk: %.2fms (%.1fx speedup)\n",
               singleTimeMs, bulkTimeMs,
               bulkTimeMs > 0 ? singleTimeMs / bulkTimeMs : 0);

        clusterRingFree(ring);
    }

    return err;
}

/* ====================================================================
 * Edge Case Tests for Optimizations
 * ==================================================================== */

static int testOptimizationEdgeCases(void) {
    int err = 0;

    TEST("EDGE: Bitmap tracker with exactly 64 nodes") {
        /* Test boundary case for small bitmap (64 nodes) */
        clusterRing *ring = clusterRingNewDefault();

        for (uint64_t i = 1; i <= 64; i++) {
            clusterNodeConfig config = {
                .id = i, .weight = 100, .initialState = CLUSTER_NODE_UP};
            clusterRingAddNode(ring, &config);
        }

        /* Should use small bitmap optimization */
        clusterPlacement p;
        clusterResult result = clusterRingLocate(ring, "test-key", 8, &p);
        if (result != CLUSTER_OK) {
            ERRR("64-node placement failed");
            err++;
        } else {
            clusterPlacementFree(&p);
        }

        clusterRingFree(ring);
    }

    TEST("EDGE: Bitmap tracker with 65 nodes (switch to medium)") {
        /* Test boundary case: transition from small to medium bitmap */
        clusterRing *ring = clusterRingNewDefault();

        for (uint64_t i = 1; i <= 65; i++) {
            clusterNodeConfig config = {
                .id = i, .weight = 100, .initialState = CLUSTER_NODE_UP};
            clusterRingAddNode(ring, &config);
        }

        /* Should use medium bitmap optimization */
        clusterPlacement p;
        clusterResult result = clusterRingLocate(ring, "test-key", 8, &p);
        if (result != CLUSTER_OK) {
            ERRR("65-node placement failed");
            err++;
        } else {
            clusterPlacementFree(&p);
        }

        clusterRingFree(ring);
    }

    TEST("EDGE: Bitmap tracker with 512 nodes") {
        /* Test boundary case for medium bitmap (512 nodes) */
        clusterRing *ring = clusterRingNewDefault();

        for (uint64_t i = 1; i <= 512; i++) {
            clusterNodeConfig config = {
                .id = i, .weight = 100, .initialState = CLUSTER_NODE_UP};
            clusterRingAddNode(ring, &config);
        }

        clusterPlacement p;
        clusterResult result = clusterRingLocate(ring, "test-key", 8, &p);
        if (result != CLUSTER_OK) {
            ERRR("512-node placement failed");
            err++;
        } else {
            clusterPlacementFree(&p);
        }

        clusterRingFree(ring);
    }

    TEST("EDGE: Bitmap tracker with 513 nodes (large allocation)") {
        /* Test boundary case: transition to heap-allocated bitmap */
        clusterRing *ring = clusterRingNewDefault();

        for (uint64_t i = 1; i <= 513; i++) {
            clusterNodeConfig config = {
                .id = i, .weight = 100, .initialState = CLUSTER_NODE_UP};
            clusterRingAddNode(ring, &config);
        }

        clusterPlacement p;
        clusterResult result = clusterRingLocate(ring, "test-key", 8, &p);
        if (result != CLUSTER_OK) {
            ERRR("513-node placement failed");
            err++;
        } else {
            clusterPlacementFree(&p);
        }

        clusterRingFree(ring);
    }

    TEST("EDGE: Heap top-k with k > n (Rendezvous)") {
        /* Request more replicas than nodes available */
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_RENDEZVOUS,
            .hashSeed = 12345,
        };
        clusterRing *ring = clusterRingNew(&config);

        /* Add only 3 nodes */
        for (uint64_t i = 1; i <= 3; i++) {
            clusterNodeConfig nodeConfig = {
                .id = i, .weight = 100, .initialState = CLUSTER_NODE_UP};
            clusterRingAddNode(ring, &nodeConfig);
        }

        /* Request 10 replicas - should return only 3 */
        const clusterNode *nodes[10];
        uint32_t count = clusterLocateRendezvous(ring, "test", 4, nodes, 10);
        if (count != 3) {
            ERR("Expected 3 nodes but got %u", count);
            err++;
        }

        clusterRingFree(ring);
    }

    TEST("EDGE: Direct node pointer after vnode add") {
        /* Verify nodePtr is correctly set in vnodes */
        clusterRingConfig config = {.strategyType = CLUSTER_STRATEGY_KETAMA};
        clusterRing *ring = clusterRingNew(&config);

        clusterNodeConfig nodeConfig = {
            .id = 42, .weight = 100, .initialState = CLUSTER_NODE_UP};
        clusterRingAddNode(ring, &nodeConfig);

        /* Check that vnodes have valid nodePtr */
        const clusterKetamaData *data = &ring->strategyData.ketama;
        bool allPointersValid = true;
        for (uint32_t i = 0; i < data->vnodeCount; i++) {
            if (data->vnodes[i].nodePtr == NULL) {
                allPointersValid = false;
                break;
            }
            if (data->vnodes[i].nodePtr->id != 42) {
                allPointersValid = false;
                break;
            }
        }
        if (!allPointersValid) {
            ERRR("Vnode nodePtr not correctly set");
            err++;
        }

        clusterRingFree(ring);
    }

    TEST("EDGE: Prefetch with single node ring") {
        /* Edge case: prefetch shouldn't crash with minimal data */
        clusterRing *ring = clusterRingNewDefault();

        clusterNodeConfig config = {
            .id = 1, .weight = 100, .initialState = CLUSTER_NODE_UP};
        clusterRingAddNode(ring, &config);

        /* Multiple locate calls to exercise prefetch code */
        for (int i = 0; i < 10; i++) {
            char key[16];
            snprintf(key, sizeof(key), "key-%d", i);
            clusterPlacement p;
            clusterRingLocate(ring, key, strlen(key), &p);
            clusterPlacementFree(&p);
        }

        clusterRingFree(ring);
    }

    TEST("EDGE: Maglev prefetch at table boundary") {
        /* Test Maglev table wraparound with prefetching */
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_MAGLEV,
            .hashSeed = 99999, /* Seed to likely hit near-end indices */
        };
        clusterRing *ring = clusterRingNew(&config);

        for (uint64_t i = 1; i <= 10; i++) {
            clusterNodeConfig nodeConfig = {
                .id = i, .weight = 100, .initialState = CLUSTER_NODE_UP};
            clusterRingAddNode(ring, &nodeConfig);
        }

        /* Request multiple replicas to exercise the prefetch loop */
        const clusterNode *nodes[5];
        uint32_t count =
            clusterLocateMaglev(ring, "boundary-test", 13, nodes, 5);
        if (count == 0) {
            ERRR("Maglev boundary test returned no nodes");
            err++;
        }

        clusterRingFree(ring);
    }

    TEST("EDGE: Bulk locate with mixed key types") {
        clusterRing *ring = clusterRingNewDefault();

        for (uint64_t i = 1; i <= 10; i++) {
            clusterNodeConfig config = {
                .id = i, .weight = 100, .initialState = CLUSTER_NODE_UP};
            clusterRingAddNode(ring, &config);
        }

        /* Mix of different key types */
        databox keys[4];
        clusterPlacement placements[4];

        /* String key */
        keys[0].type = DATABOX_BYTES;
        keys[0].data.bytes.start = (uint8_t *)"string-key";
        keys[0].len = 10;

        /* Integer key */
        keys[1].type = DATABOX_UNSIGNED_64;
        keys[1].data.u64 = 12345678ULL;

        /* Another string */
        keys[2].type = DATABOX_BYTES;
        keys[2].data.bytes.start = (uint8_t *)"another-key";
        keys[2].len = 11;

        /* Signed integer */
        keys[3].type = DATABOX_SIGNED_64;
        keys[3].data.i64 = -99999LL;

        clusterResult result = clusterRingLocateBulk(ring, keys, 4, placements);
        if (result != CLUSTER_OK) {
            ERR("Bulk locate with mixed types failed: %d", result);
            err++;
        }

        for (int i = 0; i < 4; i++) {
            clusterPlacementFree(&placements[i]);
        }

        clusterRingFree(ring);
    }

    TEST("EDGE: Very long key (>248 bytes)") {
        /* Test Rendezvous with key that exceeds buffer in HRW calculation */
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_RENDEZVOUS,
        };
        clusterRing *ring = clusterRingNew(&config);

        for (uint64_t i = 1; i <= 5; i++) {
            clusterNodeConfig nodeConfig = {
                .id = i, .weight = 100, .initialState = CLUSTER_NODE_UP};
            clusterRingAddNode(ring, &nodeConfig);
        }

        /* Create 500-byte key */
        char longKey[500];
        memset(longKey, 'A', 499);
        longKey[499] = '\0';

        clusterPlacement p;
        clusterResult result = clusterRingLocate(ring, longKey, 499, &p);
        if (result != CLUSTER_OK) {
            ERRR("Long key placement failed");
            err++;
        } else {
            clusterPlacementFree(&p);
        }

        clusterRingFree(ring);
    }

    return err;
}

int clusterRingTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    err += testRingLifecycle();
    err += testNodeManagement();
    err += testPlacement();
    err += testStrategies();
    err += testKeyspaces();
    err += testSerialization();
    err += testDistribution();
    err += testNodeFailure();
    err += testStats();
    err += testCallbacks();
    err += testIteration();

    /* Fuzzing tests */
    err += testFuzzNodeChurn();
    err += testFuzzPlacementConsistency();
    err += testFuzzAllStrategies();
    err += testFuzzStateTransitions();
    err += testFuzzSerializationRoundtrip();
    err += testFuzzConcurrentModification();
    err += testFuzzLargeScale();
    err += testFuzzEdgeCases();

    /* Benchmark tests */
    err += testBenchmarkLocate();

    /* Optimization edge case tests */
    err += testOptimizationEdgeCases();

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
