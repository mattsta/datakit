/* clusterRing.c - Consistent Hashing State Machine Implementation
 *
 * A system-level abstraction for data-structure-driven consistent hashing
 * logic providing topology-aware placement, quorum semantics, and pluggable
 * algorithms without implementing network I/O.
 */

#include "clusterRing.h"
#include "clusterRingInternal.h"

#include "../deps/xxHash/xxhash.h"
#include "cleaner.h"
#include "datakit.h"
#include "flex.h"
#include "persist.h"
#include "timeUtil.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
static void clusterRebalancePlanFree(clusterRebalancePlan *plan);
static flex *clusterRingBuildSnapshotFlex(const clusterRing *ring);
defcleaner(clusterRing *, clusterRingFree);
defcleaner(clusterRebalancePlan *, clusterRebalancePlanFree);
defcleaner(void *, zfree);
defcleanerName(uint64_t *, zfree, U64);
defcleanerName(uint32_t *, zfree, U32);
defcleanerName(const clusterNode **, zfree, NodePtrs);
defcleanerName(clusterAffinityRule *, zfree, Rules);
defcleanerName(char *, zfree, Chars);
defcleanerName(bool *, zfree, Bools);

/* Strategy state (see "Strategy State" below) */
static clusterResult clusterRingUseStrategy(clusterRing *ring,
                                            clusterStrategyType strategy);
static void clusterRingReleaseStrategy(clusterRing *ring,
                                       clusterStrategyType strategy);
static void clusterRingFreeVnodes(clusterRing *ring);
static void clusterRingFreeJump(clusterRing *ring);
static void clusterRingFreeMaglev(clusterRing *ring);
static void clusterRingRebuildJump(clusterRing *ring);
static void clusterRingRebuildMaglev(clusterRing *ring);

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
        /* @own: container; free=seenTrackerFree */
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

/* O(1) nodeId -> nodeArray index via the existing nodeById dict (which already
 * maps id -> node*), reading the node's maintained arrayIndex. Called per
 * slot/vnode/replica in the routing hot paths, so the old O(nodeCount) scan
 * was effectively quadratic in node count. */
static inline int32_t nodeIdToIndex(const clusterRing *ring, uint64_t nodeId) {
    databox keybox = databoxNewUnsigned(nodeId);
    databox valbox;
    if (!multidictFind(ring->nodeById, &keybox, &valbox)) {
        return -1;
    }
    return (int32_t)((const clusterNode *)valbox.data.ptr)->arrayIndex;
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

defcleanerName(heapNode *, zfree, HeapNodes);

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

/* Latency histogram bucket of `ns`: values below CLUSTER_LATENCY_SUB have
 * their own bucket; above, each power of two splits into
 * CLUSTER_LATENCY_SUB equal buckets */
static uint32_t clusterLatencyBucket(uint64_t ns) {
    if (ns < CLUSTER_LATENCY_SUB) {
        return (uint32_t)ns;
    }
    const uint32_t msb = 63u - (uint32_t)__builtin_clzll(ns);
    const uint32_t shift = msb - CLUSTER_LATENCY_SUB_BITS;
    return (msb - CLUSTER_LATENCY_SUB_BITS + 1u) * CLUSTER_LATENCY_SUB +
           (uint32_t)((ns >> shift) & (CLUSTER_LATENCY_SUB - 1u));
}

/* The largest value bucket `b` holds */
static uint64_t clusterLatencyBucketMax(uint32_t b) {
    if (b < CLUSTER_LATENCY_SUB) {
        return b;
    }
    const uint32_t shift = b / CLUSTER_LATENCY_SUB - 1u;
    const uint64_t lowest =
        (uint64_t)(CLUSTER_LATENCY_SUB + b % CLUSTER_LATENCY_SUB) << shift;
    return lowest + (((uint64_t)1 << shift) - 1u);
}

static void clusterLatencyRecord(clusterLatency *lat, uint64_t ns) {
    atomic_fetch_add_explicit(&lat->count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&lat->sumNs, ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&lat->buckets[clusterLatencyBucket(ns)], 1,
                              memory_order_relaxed);
    uint64_t max = atomic_load_explicit(&lat->maxNs, memory_order_relaxed);
    while (ns > max && !atomic_compare_exchange_weak_explicit(
                           &lat->maxNs, &max, ns, memory_order_relaxed,
                           memory_order_relaxed)) {
    }
}

/* Mean, 99th percentile (the upper edge of the bucket holding it, so at
 * most 1/CLUSTER_LATENCY_SUB above the true value) and maximum of the
 * recorded samples; all 0 before the first */
static void clusterLatencySummary(const clusterLatency *lat, uint64_t *avg,
                                  uint64_t *p99, uint64_t *max) {
    const uint64_t count =
        atomic_load_explicit(&lat->count, memory_order_relaxed);
    *avg = *p99 = *max = 0;
    if (count == 0) {
        return;
    }

    *avg = atomic_load_explicit(&lat->sumNs, memory_order_relaxed) / count;
    *max = atomic_load_explicit(&lat->maxNs, memory_order_relaxed);

    /* The sample ranked ceil(0.99 * count); during concurrent placements
     * the walk also sees samples recorded after `count` was read, so it is
     * off by at most those */
    const uint64_t rank = count - count / 100;
    uint64_t seen = 0;
    for (uint32_t b = 0; b < CLUSTER_LATENCY_BUCKETS; b++) {
        seen += atomic_load_explicit(&lat->buckets[b], memory_order_relaxed);
        if (seen >= rank) {
            const uint64_t edge = clusterLatencyBucketMax(b);
            *p99 = edge < *max ? edge : *max;
            return;
        }
    }
    *p99 = *max;
}

/* ====================================================================
 * Placement/Set Helper Functions
 * ==================================================================== */

/* Appends a node; the list grows geometrically from 4 entries */
static void clusterNodeListAdd(clusterNodeList *list, const clusterNode *node) {
    if (!node) {
        return;
    }
    if (list->count == list->capacity) {
        const uint32_t capacity = list->capacity ? list->capacity * 2 : 4;
        /* @own: container; free=zfree */
        list->nodes = zrealloc(list->nodes, capacity * sizeof(*list->nodes));
        list->capacity = capacity;
    }
    list->nodes[list->count++] = node;
}

static void clusterNodeListFree(clusterNodeList *list) {
    zfree(list->nodes);
    *list = (clusterNodeList){0};
}

static const clusterNode *clusterNodeListGet(const clusterNodeList *list,
                                             size_t idx) {
    return idx < list->count ? list->nodes[idx] : NULL;
}

void clusterPlacementInit(clusterPlacement *p) {
    if (p) {
        memset(p, 0, sizeof(*p));
    }
}

void clusterPlacementFree(clusterPlacement *p) {
    if (p) {
        clusterNodeListFree(&p->replicas);
    }
}

size_t clusterPlacementReplicaCount(const clusterPlacement *p) {
    return p ? p->replicas.count : 0;
}

const clusterNode *clusterPlacementGetReplica(const clusterPlacement *p,
                                              size_t idx) {
    return p ? clusterNodeListGet(&p->replicas, idx) : NULL;
}

static void clusterPlacementAddReplica(clusterPlacement *p,
                                       const clusterNode *node) {
    clusterNodeListAdd(&p->replicas, node);
}

void clusterWriteSetInit(clusterWriteSet *ws) {
    if (ws) {
        memset(ws, 0, sizeof(*ws));
    }
}

void clusterWriteSetFree(clusterWriteSet *ws) {
    if (ws) {
        clusterNodeListFree(&ws->targets);
    }
}

size_t clusterWriteSetTargetCount(const clusterWriteSet *ws) {
    return ws ? ws->targets.count : 0;
}

const clusterNode *clusterWriteSetGetTarget(const clusterWriteSet *ws,
                                            size_t idx) {
    return ws ? clusterNodeListGet(&ws->targets, idx) : NULL;
}

static void clusterWriteSetAddTarget(clusterWriteSet *ws,
                                     const clusterNode *node) {
    clusterNodeListAdd(&ws->targets, node);
}

void clusterReadSetInit(clusterReadSet *rs) {
    if (rs) {
        memset(rs, 0, sizeof(*rs));
    }
}

void clusterReadSetFree(clusterReadSet *rs) {
    if (rs) {
        clusterNodeListFree(&rs->candidates);
    }
}

size_t clusterReadSetCandidateCount(const clusterReadSet *rs) {
    return rs ? rs->candidates.count : 0;
}

const clusterNode *clusterReadSetGetCandidate(const clusterReadSet *rs,
                                              size_t idx) {
    return rs ? clusterNodeListGet(&rs->candidates, idx) : NULL;
}

static void clusterReadSetAddCandidate(clusterReadSet *rs,
                                       const clusterNode *node) {
    clusterNodeListAdd(&rs->candidates, node);
}

/* ====================================================================
 * Node Management Helpers
 * ==================================================================== */

static clusterNode *clusterNodeAlloc(void) {
    /* @own: caller; free=clusterNodeFreeInternal */
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
    /* @own: container; free=mdscfree */
    node->name = config->name ? mdscnew(config->name) : mdscempty();
    /* @own: container; free=mdscfree */
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
 * Configuration checks (shared by the constructors, restore and the
 * persist validate hook)
 * ==================================================================== */

static bool clusterRingValidNodeState(uint64_t v) {
    return v <= (uint64_t)CLUSTER_NODE_MAINTENANCE;
}

static bool clusterRingValidStrategyType(uint64_t v) {
    return v <= (uint64_t)CLUSTER_STRATEGY_CUSTOM;
}

static bool clusterRingValidConsistencyLevel(uint64_t v) {
    return v <= (uint64_t)CLUSTER_CONSISTENCY_LOCAL_ONE;
}

static bool clusterRingValidTopologyLevel(uint64_t v) {
    return v < (uint64_t)CLUSTER_LEVEL_COUNT;
}

static bool clusterRingValidQuorum(const clusterQuorum *q) {
    if (!clusterRingValidConsistencyLevel((uint64_t)q->consistency)) {
        return false;
    }

    return q->replicaCount == 0 || (q->writeQuorum <= q->replicaCount &&
                                    q->writeSync <= q->replicaCount &&
                                    q->readQuorum <= q->replicaCount);
}

static bool clusterRingValidRules(const clusterAffinityRule *rules,
                                  uint8_t count) {
    for (uint8_t r = 0; rules && r < count; r++) {
        if (!clusterRingValidTopologyLevel((uint64_t)rules[r].spreadLevel)) {
            return false;
        }
    }

    return true;
}

/* ====================================================================
 * Ring Lifecycle
 * ==================================================================== */

clusterRing *clusterRingNew(const clusterRingConfig *config) {
    if (!config ||
        !clusterRingValidStrategyType((uint64_t)config->strategyType) ||
        !clusterRingValidRules(config->affinityRules,
                               config->affinityRuleCount) ||
        (config->defaultQuorum.replicaCount &&
         !clusterRingValidQuorum(&config->defaultQuorum)) ||
        (config->boundedLoadPercent != 0 && config->boundedLoadPercent < 100)) {
        return NULL;
    }

    /* Vnode bounds after defaults: min <= max <= the per-node limit */
    const clusterVnodeConfig *vc = &config->vnodes;
    const uint32_t vnodeMin =
        vc->minVnodesPerNode ? vc->minVnodesPerNode : MIN_VNODES_PER_NODE;
    const uint32_t vnodeMax =
        vc->maxVnodesPerNode ? vc->maxVnodesPerNode : MAX_VNODES_PER_NODE;
    if (vnodeMin > vnodeMax || vnodeMax > CLUSTER_VNODES_PER_NODE_MAX) {
        return NULL;
    }

    /* A zeroed ring is safe to hand to clusterRingFree at ANY point
     * during construction (every teardown arm NULL-checks), so the
     * cleaner covers all partial-construction exits below. */
    cleaner(clusterRingFree) clusterRing *ring = zcalloc(1, sizeof(*ring));
    if (!ring) {
        return NULL;
    }

    /* Convert const char * to mdsc internally */
    /* @own: container; free=mdscfree */
    ring->name = config->name ? mdscnew(config->name) : mdscempty();

    /* Initialize node storage */
    ring->nodeCapacity = config->expectedNodeCount > 0
                             ? config->expectedNodeCount
                             : INITIAL_NODE_CAPACITY;
    /* @own: container; free=clusterRingFree */
    ring->nodeArray = zcalloc(ring->nodeCapacity, sizeof(clusterNode *));
    if (!ring->nodeArray) {
        return NULL;
    }
    ring->nodeCount = 0;
    ring->healthyNodeCount = 0;

    /* Create node lookup dict. The dict privately owns the default class
     * it is built with; clusterRingFree releases both in order. */
    /* @own: container; free=multidictDefaultClassFree */
    multidictClass *nodeClass = multidictDefaultClassNew();
    /* @own: container; free=multidictFree */
    ring->nodeById = multidictNew(&multidictTypeExactKey, nodeClass, 0);
    if (!ring->nodeById) {
        return NULL;
    }

    /* Strategy configuration (a ring's DEFAULT is Ketama) */
    ring->strategyType = config->strategyType == CLUSTER_STRATEGY_DEFAULT
                             ? CLUSTER_STRATEGY_KETAMA
                             : config->strategyType;
    ring->customStrategy = config->customStrategy;
    ring->hashSeed = config->hashSeed;
    ring->boundedLoadPercent =
        config->boundedLoadPercent ? config->boundedLoadPercent : 125;

    /* Virtual node configuration */
    ring->vnodeConfig = config->vnodes;
    if (ring->vnodeConfig.vnodeMultiplier == 0) {
        ring->vnodeConfig.vnodeMultiplier = DEFAULT_VNODE_MULTIPLIER;
    }
    ring->vnodeConfig.minVnodesPerNode = vnodeMin;
    ring->vnodeConfig.maxVnodesPerNode = vnodeMax;

    /* Default quorum */
    ring->defaultQuorum = config->defaultQuorum;
    if (ring->defaultQuorum.replicaCount == 0) {
        ring->defaultQuorum = CLUSTER_QUORUM_BALANCED;
    }

    /* Copy affinity rules */
    if (config->affinityRules && config->affinityRuleCount > 0) {
        /* @own: container; free=clusterRingFree */
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
    /* @own: container; free=clusterRingFree */
    ring->keySpaces =
        zcalloc(ring->keySpaceCapacity, sizeof(clusterKeySpace *));
    /* @own: container; free=multidictDefaultClassFree */
    multidictClass *keySpaceClass = multidictDefaultClassNew();
    /* @own: container; free=multidictFree */
    ring->keySpaceByName =
        multidictNew(&multidictTypeExactKey, keySpaceClass, 0);

    /* The ring is its own strategy's first user */
    if (clusterRingUseStrategy(ring, ring->strategyType) != CLUSTER_OK) {
        return NULL;
    }

    /* Initialize version tracking */
    ring->version = 1;
    ring->lastModified = clusterGetTimeMs();

    clusterRing *out = ring;
    ring = NULL;
    return out;
}

clusterRing *clusterRingNewDefault(void) {
    clusterRingConfig config = {
        .name = "default",
        .strategyType = CLUSTER_STRATEGY_KETAMA,
        .defaultQuorum = CLUSTER_QUORUM_BALANCED,
    };
    /* @own: caller; free=clusterRingFree */
    return clusterRingNew(&config);
}

void clusterRingFree(clusterRing *ring) {
    if (!ring) {
        return;
    }

    /* Free every live strategy's state (and the custom hooks' data) */
    clusterRingFreeVnodes(ring);
    clusterRingFreeJump(ring);
    clusterRingFreeMaglev(ring);
    if (ring->strategyType == CLUSTER_STRATEGY_CUSTOM && ring->customStrategy &&
        !ring->sharesCustomStrategy && ring->customStrategy->freeStrategyData) {
        ring->customStrategy->freeStrategyData(
            ring->customStrategy->strategyData);
    }

    /* Free nodes */
    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        if (ring->nodeArray[i]) {
            clusterNodeFreeInternal(ring->nodeArray[i]);
        }
    }
    zfree(ring->nodeArray);

    /* Free node lookup. The ring privately owns each dict's default
     * class; the class must outlive the dict (teardown runs class
     * callbacks), so free it after. */
    if (ring->nodeById) {
        multidictClass *cls = multidictGetClass(ring->nodeById);
        multidictFree(ring->nodeById);
        multidictDefaultClassFree(cls);
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
        multidictClass *cls = multidictGetClass(ring->keySpaceByName);
        multidictFree(ring->keySpaceByName);
        multidictDefaultClassFree(cls);
    }

    /* Free affinity rules */
    if (ring->affinityRules) {
        zfree(ring->affinityRules);
    }

    clusterRebalancePlanFree(ring->rebalancePlan);

    clusterRingSetHealthProvider(ring, NULL);

    /* Free ring name */
    if (ring->name) {
        mdscfree(ring->name);
    }

    zfree(ring);
}

/* ====================================================================
 * Strategy State
 * ==================================================================== */

static bool clusterRingHasVnodes(const clusterRing *ring) {
    return ring->strategyUsers[CLUSTER_STRATEGY_KETAMA] +
               ring->strategyUsers[CLUSTER_STRATEGY_BOUNDED] >
           0;
}

static bool clusterRingHasJump(const clusterRing *ring) {
    return ring->strategyUsers[CLUSTER_STRATEGY_JUMP] > 0;
}

static bool clusterRingHasMaglev(const clusterRing *ring) {
    return ring->strategyUsers[CLUSTER_STRATEGY_MAGLEV] > 0;
}

/* The strategy a user places with: a keyspace's DEFAULT is the ring's */
static clusterStrategyType
clusterRingResolveStrategy(const clusterRing *ring,
                           clusterStrategyType strategy) {
    return strategy == CLUSTER_STRATEGY_DEFAULT ? ring->strategyType : strategy;
}

/* Build the vnode ring over every node (the vnode users are counted) */
static clusterResult clusterRingBuildVnodes(clusterRing *ring) {
    clusterKetamaData *data = &ring->vnodes;
    data->vnodeCapacity = INITIAL_VNODE_CAPACITY;
    /* @own: container; free=clusterRingFreeVnodes */
    data->vnodes = zcalloc(data->vnodeCapacity, sizeof(clusterVnode));
    if (!data->vnodes) {
        return CLUSTER_ERR_ALLOC_FAILED;
    }
    data->vnodeCount = 0;
    data->needsSort = false;
    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        const clusterResult r = clusterRingAddVnodes(ring, ring->nodeArray[i]);
        if (r != CLUSTER_OK) {
            return r;
        }
    }
    clusterRingSortVnodes(ring);
    return CLUSTER_OK;
}

static void clusterRingFreeVnodes(clusterRing *ring) {
    zfree(ring->vnodes.vnodes);
    memset(&ring->vnodes, 0, sizeof(ring->vnodes));
}

static void clusterRingFreeJump(clusterRing *ring) {
    zfree(ring->jump.nodes);
    memset(&ring->jump, 0, sizeof(ring->jump));
}

static void clusterRingFreeMaglev(clusterRing *ring) {
    if (ring->maglev.lookup) {
        pthread_mutex_destroy(&ring->maglev.lock);
        zfree(ring->maglev.lookup);
    }
    memset(&ring->maglev, 0, sizeof(ring->maglev));
}

/* After any change to membership or health: rebuild Jump's buckets and
 * mark Maglev's table stale (when live; its next placement rebuilds it) */
static void clusterRingRefreshHealthy(clusterRing *ring) {
    if (clusterRingHasJump(ring)) {
        clusterRingRebuildJump(ring);
    }
    if (clusterRingHasMaglev(ring)) {
        atomic_store_explicit(&ring->maglev.stale, true, memory_order_release);
    }
}

/* Rebuild a stale Maglev table before a placement reads it: concurrent
 * placements wait for one rebuild (double-checked under the lock) */
static void clusterRingSettleMaglev(const clusterRing *ring) {
    clusterMaglevData *data = (clusterMaglevData *)&ring->maglev;
    if (!atomic_load_explicit(&data->stale, memory_order_acquire)) {
        return;
    }

    pthread_mutex_lock(&data->lock);
    if (atomic_load_explicit(&data->stale, memory_order_relaxed)) {
        clusterRingRebuildMaglev((clusterRing *)ring);
        atomic_store_explicit(&data->stale, false, memory_order_release);
    }
    pthread_mutex_unlock(&data->lock);
}

/* Count a user (the ring, or a keyspace) of `strategy`; the first user of
 * a strategy that keeps state builds it from the current nodes */
static clusterResult clusterRingUseStrategy(clusterRing *ring,
                                            clusterStrategyType strategy) {
    const bool hadVnodes = clusterRingHasVnodes(ring);
    ring->strategyUsers[strategy]++;

    if (!hadVnodes && clusterRingHasVnodes(ring)) {
        const clusterResult r = clusterRingBuildVnodes(ring);
        if (r != CLUSTER_OK) {
            ring->strategyUsers[strategy]--;
            clusterRingFreeVnodes(ring);
            return r;
        }
    }

    if (ring->strategyUsers[strategy] == 1) {
        if (strategy == CLUSTER_STRATEGY_JUMP) {
            clusterRingRebuildJump(ring);
        } else if (strategy == CLUSTER_STRATEGY_MAGLEV) {
            /* Built by its first placement */
            ring->maglev.tableSize = MAGLEV_TABLE_SIZE;
            /* @own: container; free=clusterRingFreeMaglev */
            ring->maglev.lookup =
                zcalloc(ring->maglev.tableSize, sizeof(*ring->maglev.lookup));
            pthread_mutex_init(&ring->maglev.lock, NULL);
            atomic_store_explicit(&ring->maglev.stale, true,
                                  memory_order_release);
        }
    }

    return CLUSTER_OK;
}

/* Drop a user of `strategy`; the last user of a strategy's state frees it */
static void clusterRingReleaseStrategy(clusterRing *ring,
                                       clusterStrategyType strategy) {
    if (ring->strategyUsers[strategy] == 0) {
        return;
    }

    ring->strategyUsers[strategy]--;
    if (!clusterRingHasVnodes(ring)) {
        clusterRingFreeVnodes(ring);
    }
    if (!clusterRingHasJump(ring)) {
        clusterRingFreeJump(ring);
    }
    if (!clusterRingHasMaglev(ring)) {
        clusterRingFreeMaglev(ring);
    }
}

/* A node's fill: the share of its capacity in use (0 without a capacity) */
static double clusterNodeFill(const clusterNode *node) {
    return node->capacityBytes
               ? (double)node->usedBytes / (double)node->capacityBytes
               : 0.0;
}

/* Count a healthy node with a capacity into (sign 1) or out of (sign -1)
 * the ring's fill sum (bounded-load's mean); call around every change to
 * the node's state or usage, and on add / remove */
static void clusterBoundedAccount(clusterRing *ring, const clusterNode *node,
                                  int sign) {
    if (node->state != CLUSTER_NODE_UP || node->capacityBytes == 0) {
        return;
    }

    ring->fillSum += sign * clusterNodeFill(node);
    ring->fillNodes = (uint32_t)((int64_t)ring->fillNodes + sign);
    if (ring->fillNodes == 0) {
        ring->fillSum = 0; /* no rounding residue survives an empty set */
    }
}

/* ====================================================================
 * Virtual Node Management (Ketama)
 * ==================================================================== */

/* A total order (hash point, then node id, then vnode index), so the
 * sorted ring, and with it every placement, depends only on the ring's
 * nodes and never on the order they were added in */
static int vnodeCompare(const void *a, const void *b) {
    const clusterVnode *va = (const clusterVnode *)a;
    const clusterVnode *vb = (const clusterVnode *)b;
    if (va->hashPoint != vb->hashPoint) {
        return va->hashPoint < vb->hashPoint ? -1 : 1;
    }
    if (va->nodePtr->id != vb->nodePtr->id) {
        return va->nodePtr->id < vb->nodePtr->id ? -1 : 1;
    }
    return (int)va->vnodeIndex - (int)vb->vnodeIndex;
}

void clusterRingSortVnodes(clusterRing *ring) {
    clusterKetamaData *data = &ring->vnodes;
    if (data->needsSort && data->vnodeCount > 1) {
        qsort(data->vnodes, data->vnodeCount, sizeof(clusterVnode),
              vnodeCompare);
    }
    data->needsSort = false;
}

/* The count of leading vnodes known to be in order (all of them unless a
 * sort is pending), taken before clusterRingAddVnodes appends */
static uint32_t clusterRingSortedVnodes(const clusterRing *ring) {
    const clusterKetamaData *data = &ring->vnodes;
    return clusterRingHasVnodes(ring) && !data->needsSort ? data->vnodeCount
                                                          : 0;
}

/* Put the ring's vnodes in order after clusterRingAddVnodes appended some
 * past the first `sorted`, which are in order: the appended ones are
 * sorted and merged in from the back, O(vnodes + k log k), instead of
 * sorting the whole ring again. */
static void clusterRingMergeVnodes(clusterRing *ring, uint32_t sorted) {
    if (!clusterRingHasVnodes(ring)) {
        return;
    }

    clusterKetamaData *data = &ring->vnodes;
    const uint32_t added = data->vnodeCount - sorted;
    if (sorted == 0 || added == 0) {
        clusterRingSortVnodes(ring);
        return;
    }

    clusterVnode *const vnodes = data->vnodes;
    qsort(vnodes + sorted, added, sizeof(*vnodes), vnodeCompare);
    cleaner(zfree) void *tail = zmalloc(added * sizeof(*vnodes));
    clusterVnode *const fresh = tail;
    memcpy(fresh, vnodes + sorted, added * sizeof(*vnodes));

    uint32_t i = sorted;
    uint32_t j = added;
    uint32_t w = data->vnodeCount;
    while (j > 0) {
        if (i > 0 && vnodeCompare(&vnodes[i - 1], &fresh[j - 1]) > 0) {
            vnodes[--w] = vnodes[--i];
        } else {
            vnodes[--w] = fresh[--j];
        }
    }

    data->needsSort = false;
}

clusterResult clusterRingAddVnodes(clusterRing *ring, clusterNode *node) {
    if (!clusterRingHasVnodes(ring)) {
        return CLUSTER_OK; /* No strategy in use places on vnodes */
    }

    clusterKetamaData *data = &ring->vnodes;

    /* Number of vnodes from the weight, clamped to [min, max] (max is at
     * most CLUSTER_VNODES_PER_NODE_MAX, so every index fits vnodeIndex) */
    const uint64_t scaled =
        (uint64_t)node->weight * ring->vnodeConfig.vnodeMultiplier / 100;
    uint32_t vnodeCount = scaled > ring->vnodeConfig.maxVnodesPerNode
                              ? ring->vnodeConfig.maxVnodesPerNode
                              : (uint32_t)scaled;
    if (vnodeCount < ring->vnodeConfig.minVnodesPerNode) {
        vnodeCount = ring->vnodeConfig.minVnodesPerNode;
    }

    /* Grow vnode array if needed (the total stays a uint32_t) */
    const uint64_t need = (uint64_t)data->vnodeCount + vnodeCount;
    if (need > UINT32_MAX) {
        return CLUSTER_ERR_ALLOC_FAILED;
    }

    if (need > data->vnodeCapacity) {
        uint64_t newCapacity = (uint64_t)data->vnodeCapacity * 2;
        while (newCapacity < need) {
            newCapacity *= 2;
        }

        if (newCapacity > UINT32_MAX) {
            newCapacity = UINT32_MAX;
        }
        /* @own: container; free=clusterRingFree */
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
        vn->nodePtr = node; /* Direct pointer for fast lookup */
        vn->vnodeIndex = (uint16_t)i;
    }

    data->needsSort = true;
    return CLUSTER_OK;
}

clusterResult clusterRingRemoveVnodes(clusterRing *ring, uint64_t nodeId) {
    if (!clusterRingHasVnodes(ring)) {
        return CLUSTER_OK;
    }

    clusterKetamaData *data = &ring->vnodes;

    /* Remove all vnodes belonging to this node */
    uint32_t writeIdx = 0;
    for (uint32_t readIdx = 0; readIdx < data->vnodeCount; readIdx++) {
        if (data->vnodes[readIdx].nodePtr->id != nodeId) {
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
    clusterJumpData *data = &ring->jump;

    /* Free old array */
    zfree(data->nodes);

    /* The healthy nodes, in ring order, are the buckets */
    data->bucketCount = ring->healthyNodeCount;
    if (data->bucketCount == 0) {
        data->nodes = NULL;
        return;
    }

    /* @own: container; free=clusterRingFreeJump */
    data->nodes = zcalloc(data->bucketCount, sizeof(*data->nodes));
    if (!data->nodes) {
        data->bucketCount = 0;
        return;
    }

    uint32_t idx = 0;
    for (uint32_t i = 0; i < ring->nodeCount && idx < data->bucketCount; i++) {
        const clusterNode *node = ring->nodeArray[i];
        if (node && node->state == CLUSTER_NODE_UP) {
            data->nodes[idx++] = node;
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
    clusterMaglevData *data = &ring->maglev;
    if (!data->lookup) {
        return;
    }

    if (ring->healthyNodeCount == 0) {
        memset(data->lookup, 0, data->tableSize * sizeof(*data->lookup));
        return;
    }

    /* Collect healthy nodes */
    cleanerName(zfree, NodePtrs) const clusterNode **healthyNodes =
        zcalloc(ring->healthyNodeCount, sizeof(*healthyNodes));
    if (!healthyNodes) {
        return;
    }

    uint32_t healthyIdx = 0;
    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        const clusterNode *node = ring->nodeArray[i];
        if (node && node->state == CLUSTER_NODE_UP) {
            healthyNodes[healthyIdx++] = node;
        }
    }

    /* Build permutation table */
    uint32_t N = data->tableSize;
    uint32_t M = ring->healthyNodeCount;

    /* Each node's permutation of the table: it starts at an offset and
     * steps by a skip (both from its id).  The walk keeps each node's
     * current position and steps it by addition with one wrap, the same
     * sequence as (offset + j * skip) % N without a multiply and modulo
     * per probe. */
    cleanerName(zfree, U32) uint32_t *pos = zcalloc(M, sizeof(uint32_t));
    cleanerName(zfree, U32) uint32_t *skip = zcalloc(M, sizeof(uint32_t));
    if (!pos || !skip) {
        return;
    }

    for (uint32_t i = 0; i < M; i++) {
        const uint64_t id = healthyNodes[i]->id;
        uint64_t h1 = clusterHash64(&id, sizeof(id), ring->hashSeed);
        uint64_t h2 = clusterHash64(&id, sizeof(id), ring->hashSeed + 1);
        pos[i] = (uint32_t)(h1 % N);
        skip[i] = (uint32_t)(h2 % (N - 1)) + 1;
    }

    /* Fill lookup table using Maglev algorithm: nodes take turns claiming
     * the next free slot of their permutation */
    const clusterNode **const lookup = data->lookup;
    memset(lookup, 0, N * sizeof(*lookup));
    uint32_t filled = 0;

    while (filled < N) {
        for (uint32_t i = 0; i < M && filled < N; i++) {
            uint32_t c = pos[i];
            const uint32_t step = skip[i];
            while (lookup[c] != NULL) {
                c += step;
                if (c >= N) {
                    c -= N;
                }
            }
            lookup[c] = healthyNodes[i];
            c += step;
            if (c >= N) {
                c -= N;
            }
            pos[i] = c;
            filled++;
        }
    }
}

/* ====================================================================
 * Node Array Management
 * ==================================================================== */

clusterResult clusterRingGrowNodeArray(clusterRing *ring) {
    uint32_t newCapacity = ring->nodeCapacity * 2;
    /* @own: container; free=clusterRingFree */
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
            /* Keep arrayIndex consistent with the (possibly new) position so
             * nodeIdToIndex stays O(1) and correct after compaction. */
            ring->nodeArray[writeIdx]->arrayIndex = writeIdx;
            writeIdx++;
        }
    }
    ring->nodeCount = writeIdx;
}

/* ====================================================================
 * Node Management API
 * ==================================================================== */

/* Bring the live placement state up to date after nodes were added with
 * place = false: one sort of the vnode ring, one Jump and Maglev rebuild */
static void clusterRingPlaceNodes(clusterRing *ring) {
    if (clusterRingHasVnodes(ring)) {
        clusterRingSortVnodes(ring);
    }
    clusterRingRefreshHealthy(ring);
}

/* Add a node; with place = false the vnode sort and the Jump and Maglev
 * rebuilds are left to clusterRingPlaceNodes, so a bulk load pays for
 * them once. */
static clusterResult clusterRingAddNodeInternal(clusterRing *ring,
                                                const clusterNodeConfig *config,
                                                bool place) {
    if (!ring || !config) {
        return CLUSTER_ERR;
    }

    /* Check if node already exists */
    databox keybox = databoxNewUnsigned(config->id);
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

    /* Add to array (record its index for O(1) nodeIdToIndex) */
    node->arrayIndex = ring->nodeCount;
    ring->nodeArray[ring->nodeCount++] = node;

    /* Add to lookup dict */
    valbox.type = DATABOX_PTR;
    valbox.data.ptr = node;
    multidictAdd(ring->nodeById, &keybox, &valbox);

    /* Update healthy count */
    if (node->state == CLUSTER_NODE_UP) {
        ring->healthyNodeCount++;
    }

    /* Add virtual nodes (Ketama and bounded-load) */
    const uint32_t sortedVnodes = clusterRingSortedVnodes(ring);
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

    /* Keep the placement structures current (or leave them to
     * clusterRingPlaceNodes) */
    clusterBoundedAccount(ring, node, 1);
    if (place) {
        clusterRingMergeVnodes(ring, sortedVnodes);
        clusterRingRefreshHealthy(ring);
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

clusterResult clusterRingAddNode(clusterRing *ring,
                                 const clusterNodeConfig *config) {
    return clusterRingAddNodeInternal(ring, config, true);
}

clusterResult clusterRingRemoveNode(clusterRing *ring, uint64_t nodeId) {
    if (!ring) {
        return CLUSTER_ERR;
    }

    /* Find node */
    databox keybox = databoxNewUnsigned(nodeId);
    databox valbox;
    if (!multidictFind(ring->nodeById, &keybox, &valbox)) {
        return CLUSTER_ERR_NOT_FOUND;
    }

    clusterNode *node = (clusterNode *)valbox.data.ptr;
    clusterBoundedAccount(ring, node, -1);

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

    /* Free node, then rebuild the state that pointed at it */
    clusterNodeFreeInternal(node);
    clusterRingRefreshHealthy(ring);

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

    /* Placement is brought up to date once at the end, unless a state
     * callback (which may place keys) watches each add */
    const bool place = ring->stateCallback != NULL;
    clusterResult result = CLUSTER_OK;
    for (uint32_t i = 0; i < count && result == CLUSTER_OK; i++) {
        result = clusterRingAddNodeInternal(ring, &configs[i], place);
        if (result == CLUSTER_ERR_EXISTS) {
            result = CLUSTER_OK;
        }
    }

    clusterRingPlaceNodes(ring);
    return result;
}

clusterResult clusterRingSetNodeState(clusterRing *ring, uint64_t nodeId,
                                      clusterNodeState state) {
    if (!ring) {
        return CLUSTER_ERR;
    }

    databox keybox = databoxNewUnsigned(nodeId);
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

    clusterBoundedAccount(ring, node, -1);
    node->state = state;
    clusterBoundedAccount(ring, node, 1);
    node->stateChangedAt = clusterGetTimeMs();

    /* Rebuild the state that lists healthy nodes */
    clusterRingRefreshHealthy(ring);

    /* Invoke callback */
    if (ring->stateCallback) {
        ring->stateCallback(ring, nodeId, oldState, state,
                            ring->stateCallbackData);
    }

    ring->version++;
    ring->lastModified = clusterGetTimeMs();

    return CLUSTER_OK;
}

clusterResult clusterRingSetNodeUsedBytes(clusterRing *ring, uint64_t nodeId,
                                          uint64_t usedBytes) {
    if (!ring) {
        return CLUSTER_ERR;
    }

    databox keybox = databoxNewUnsigned(nodeId);
    databox valbox;
    if (!multidictFind(ring->nodeById, &keybox, &valbox)) {
        return CLUSTER_ERR_NOT_FOUND;
    }

    clusterNode *node = (clusterNode *)valbox.data.ptr;
    clusterBoundedAccount(ring, node, -1);
    node->usedBytes = usedBytes;
    clusterBoundedAccount(ring, node, 1);

    ring->version++;
    ring->lastModified = clusterGetTimeMs();
    return CLUSTER_OK;
}

clusterResult clusterRingSetNodeWeight(clusterRing *ring, uint64_t nodeId,
                                       uint32_t weight) {
    if (!ring) {
        return CLUSTER_ERR;
    }

    databox keybox = databoxNewUnsigned(nodeId);
    databox valbox;
    if (!multidictFind(ring->nodeById, &keybox, &valbox)) {
        return CLUSTER_ERR_NOT_FOUND;
    }

    clusterNode *node = (clusterNode *)valbox.data.ptr;

    /* 0 is the default weight, as in clusterNodeConfig */
    if (weight == 0) {
        weight = 100;
    }

    if (node->weight == weight) {
        return CLUSTER_OK;
    }

    /* For Ketama: remove old vnodes, update weight, add new vnodes */
    if (clusterRingHasVnodes(ring)) {
        clusterRingRemoveVnodes(ring, nodeId);
        node->weight = weight;
        const uint32_t sortedVnodes = clusterRingSortedVnodes(ring);
        const clusterResult vnodes = clusterRingAddVnodes(ring, node);
        clusterRingMergeVnodes(ring, sortedVnodes);
        if (vnodes != CLUSTER_OK) {
            return vnodes;
        }
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

    databox keybox = databoxNewUnsigned(nodeId);
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

static uint32_t clusterKetamaWalkFrom(const clusterRing *ring, uint32_t lo,
                                      const clusterNode **out,
                                      uint32_t maxNodes, double fillBound);

/* Walk the vnode ring clockwise from the key's hash, collecting up to
 * maxNodes distinct healthy nodes in ring order.  With fillBound >= 0, a
 * node whose fill is above it is passed over and taken only after every
 * other node (bounded-load), in the order the walk met it. */
static uint32_t clusterKetamaWalk(const clusterRing *ring, const void *key,
                                  uint32_t keyLen, const clusterNode **out,
                                  uint32_t maxNodes, double fillBound) {
    const clusterKetamaData *data = &ring->vnodes;

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

    return clusterKetamaWalkFrom(ring, lo, out, maxNodes, fillBound);
}

/* Up to maxNodes distinct healthy nodes in vnode order from vnode `lo` (a
 * key's placement starts at the first vnode at or after its hash), nodes
 * over `fillBound` (when not negative) after the rest */
static uint32_t clusterKetamaWalkFrom(const clusterRing *ring, uint32_t lo,
                                      const clusterNode **out,
                                      uint32_t maxNodes, double fillBound) {
    const clusterKetamaData *data = &ring->vnodes;

    /* Collect unique nodes using bitmap-based deduplication */
    uint32_t effectiveMax =
        maxNodes < ring->nodeCount ? maxNodes : ring->nodeCount;

    /* Use bitmap for O(1) seen tracking instead of O(n) linear search */
    seenTracker seen;
    seenTrackerInit(&seen, ring->nodeCount);

    /* Nodes over the fill bound, in walk order, for after the rest */
    const clusterNode *deferStack[64];
    cleanerName(zfree, NodePtrs) const clusterNode **deferHeap =
        (fillBound >= 0 && effectiveMax > 64)
            ? zcalloc(effectiveMax, sizeof(*deferHeap))
            : NULL;
    const clusterNode **deferred = deferHeap ? deferHeap : deferStack;
    uint32_t deferredCount = 0;

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

        /* The vnode points at its node, which knows its array slot */
        const clusterNode *node = vn->nodePtr;
        if (CLUSTER_LIKELY(node != NULL) &&
            !seenTrackerTest(&seen, node->arrayIndex)) {
            seenTrackerSet(&seen, node->arrayIndex);
            if (node->state == CLUSTER_NODE_UP) {
                if (fillBound >= 0 && clusterNodeFill(node) > fillBound) {
                    if (deferredCount < effectiveMax) {
                        deferred[deferredCount++] = node;
                    }
                } else {
                    out[found++] = node;
                }
            }
        }

        checked++;
    }

    for (uint32_t d = 0; d < deferredCount && found < effectiveMax; d++) {
        out[found++] = deferred[d];
    }

    seenTrackerFree(&seen);
    return found;
}

uint32_t clusterLocateKetama(const clusterRing *ring, const void *key,
                             uint32_t keyLen, const clusterNode **out,
                             uint32_t maxNodes) {
    return clusterKetamaWalk(ring, key, keyLen, out, maxNodes, -1.0);
}

uint32_t clusterLocateJump(const clusterRing *ring, const void *key,
                           uint32_t keyLen, const clusterNode **out,
                           uint32_t maxNodes) {
    const clusterJumpData *data = &ring->jump;

    if (CLUSTER_UNLIKELY(data->bucketCount == 0 || maxNodes == 0)) {
        return 0;
    }

    uint64_t hash = clusterHash64(key, keyLen, ring->hashSeed);

    /* Get primary bucket */
    int32_t bucket = jumpConsistentHash(hash, (int32_t)data->bucketCount);
    if (CLUSTER_UNLIKELY(bucket < 0 || (uint32_t)bucket >= data->bucketCount)) {
        return 0;
    }

    /* Use bitmap for O(1) deduplication */
    seenTracker seen;
    seenTrackerInit(&seen, ring->nodeCount);

    /* The buckets hold the nodes themselves */
    const clusterNode *primary = data->nodes[bucket];
    uint32_t found = 0;
    out[found++] = primary;
    seenTrackerSet(&seen, primary->arrayIndex);

    /* For replicas, use different hash seeds */
    for (uint32_t r = 1; r < maxNodes && found < maxNodes; r++) {
        uint64_t replicaHash = clusterHash64(key, keyLen, ring->hashSeed + r);
        int32_t replicaBucket =
            jumpConsistentHash(replicaHash, (int32_t)data->bucketCount);

        if (replicaBucket >= 0 && (uint32_t)replicaBucket < data->bucketCount) {
            const clusterNode *replica = data->nodes[replicaBucket];
            if (!seenTrackerTest(&seen, replica->arrayIndex)) {
                seenTrackerSet(&seen, replica->arrayIndex);
                out[found++] = replica;
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
    cleanerName(zfree, HeapNodes) heapNode *heap = zcalloc(k, sizeof(heapNode));
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

    return heapSize;
}

uint32_t clusterLocateMaglev(const clusterRing *ring, const void *key,
                             uint32_t keyLen, const clusterNode **out,
                             uint32_t maxNodes) {
    /* A table left stale by node changes is rebuilt once, first */
    clusterRingSettleMaglev(ring);
    const clusterMaglevData *data = &ring->maglev;

    if (CLUSTER_UNLIKELY(ring->healthyNodeCount == 0 || maxNodes == 0 ||
                         !data->lookup)) {
        return 0;
    }

    /* The table holds only healthy nodes: stop once all are found */
    uint32_t effectiveMax =
        maxNodes < ring->healthyNodeCount ? maxNodes : ring->healthyNodeCount;

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

        const clusterNode *node = data->lookup[lookupIdx];
        if (CLUSTER_LIKELY(node != NULL) &&
            !seenTrackerTest(&seen, node->arrayIndex)) {
            seenTrackerSet(&seen, node->arrayIndex);
            out[found++] = node;
        }
    }

    seenTrackerFree(&seen);
    return found;
}

uint32_t clusterLocateBounded(const clusterRing *ring, const void *key,
                              uint32_t keyLen, const clusterNode **out,
                              uint32_t maxNodes) {
    /* Ketama order, passing over nodes filled above boundedLoadPercent of
     * the mean fill; with no fill recorded there is nothing to pass over */
    const double mean =
        ring->fillNodes ? ring->fillSum / (double)ring->fillNodes : 0.0;
    const double bound =
        mean > 0 ? mean * (double)ring->boundedLoadPercent / 100.0 : -1.0;
    return clusterKetamaWalk(ring, key, keyLen, out, maxNodes, bound);
}

/* ====================================================================
 * Core Placement API
 * ==================================================================== */

static uint64_t getLocationValue(const clusterLocation *loc,
                                 clusterTopologyLevel level);

/* Whether `node`'s location value at `level` differs from every picked
 * node's */
static bool clusterValueIsNew(const clusterNode *node,
                              clusterTopologyLevel level,
                              const clusterNode *const *picked,
                              uint32_t pickedCount) {
    const uint64_t v = getLocationValue(&node->location, level);
    for (uint32_t p = 0; p < pickedCount; p++) {
        if (getLocationValue(&picked[p]->location, level) == v) {
            return false;
        }
    }
    return true;
}

/* Choose `want` of the `count` candidates (strategy preference order) to
 * meet `rules`: candidates that add a location value some rule still
 * needs (at its level, below its minSpread) are taken first, most
 * preferred first; then the most preferred of the rest.  The chosen keep
 * preference order in `out`.  Returns how many were chosen; *requiredMet
 * / *allMet say whether the required / all rules reached minSpread. */
static uint32_t clusterSelectReplicas(const clusterNode *const *cand,
                                      uint32_t count, uint32_t want,
                                      const clusterAffinityRule *rules,
                                      uint8_t ruleCount,
                                      const clusterNode **out,
                                      bool *requiredMet, bool *allMet) {
    bool chosenStack[64];
    cleanerName(zfree, Bools) bool *chosenHeap =
        count > 64 ? zcalloc(count, sizeof(bool)) : NULL;
    bool *chosen = count > 64 ? chosenHeap : chosenStack;
    memset(chosen, 0, (count > 64 ? count : 64) * sizeof(bool));

    uint32_t distinct[256] = {0}; /* per rule, distinct values chosen */
    const clusterNode *picked[256];
    uint32_t pickedCount = 0;

    /* Spread pass: take what a rule still needs */
    for (uint32_t i = 0; i < count && pickedCount < want; i++) {
        bool needed = false;
        for (uint8_t r = 0; r < ruleCount && !needed; r++) {
            needed = distinct[r] < rules[r].minSpread &&
                     clusterValueIsNew(cand[i], rules[r].spreadLevel, picked,
                                       pickedCount);
        }
        if (!needed) {
            continue;
        }
        for (uint8_t r = 0; r < ruleCount; r++) {
            distinct[r] += clusterValueIsNew(cand[i], rules[r].spreadLevel,
                                             picked, pickedCount);
        }
        chosen[i] = true;
        picked[pickedCount++] = cand[i];
    }

    /* Fill pass: the most preferred of the rest */
    for (uint32_t i = 0; i < count && pickedCount < want; i++) {
        if (!chosen[i]) {
            chosen[i] = true;
            pickedCount++;
        }
    }

    *requiredMet = true;
    *allMet = true;
    for (uint8_t r = 0; r < ruleCount; r++) {
        if (distinct[r] < rules[r].minSpread) {
            *allMet = false;
            *requiredMet = *requiredMet && !rules[r].required;
        }
    }

    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (chosen[i]) {
            out[n++] = cand[i];
        }
    }
    return n;
}

static clusterResult clusterRingSelect(const clusterRing *ring,
                                       const clusterNode **replicas,
                                       uint32_t *foundOut, uint32_t maxReplicas,
                                       uint32_t replicaCount,
                                       const clusterAffinityRule *rules,
                                       uint8_t ruleCount, bool *spreadMet);

/* Places `key` with an explicit strategy (resolved, its state live),
 * replica count and rule set */
static clusterResult
clusterRingPlace(const clusterRing *ring, clusterStrategyType strategy,
                 const void *key, uint32_t keyLen, uint32_t replicaCount,
                 const clusterAffinityRule *rules, uint8_t ruleCount,
                 clusterPlacement *out) {
    clusterPlacementInit(out);
    out->hashValue = clusterHash64(key, keyLen, ring->hashSeed);

    if (ring->nodeCount == 0) {
        return CLUSTER_ERR_NO_NODES;
    }

    /* With affinity rules the strategy ranks every node, so the selection
     * can reach past the first replicaCount for the spread it needs.
     * Replica sets are bounded by the picked-node scratch (256). */
    if (replicaCount > 256) {
        replicaCount = 256;
    }
    const bool spread = rules && ruleCount > 0;
    uint32_t maxReplicas = spread ? ring->nodeCount : replicaCount;

    /* Temporary stack buffer for strategy results */
    const clusterNode *stackReplicas[64];
    const clusterNode **replicas = stackReplicas;
    cleanerName(zfree, NodePtrs) const clusterNode **heapReplicas = NULL;

    if (maxReplicas > 64) {
        heapReplicas = zcalloc(maxReplicas, sizeof(const clusterNode *));
        if (!heapReplicas) {
            clusterPlacementFree(out);
            return CLUSTER_ERR_ALLOC_FAILED;
        }
        replicas = heapReplicas;
    }

    uint32_t found = 0;
    const clusterStrategy *custom = ring->customStrategy;

    switch (strategy) {
    case CLUSTER_STRATEGY_DEFAULT:
    case CLUSTER_STRATEGY_COUNT:
        break; /* resolved before placement; never reached */
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
        /* The full ranking comes from preferenceOrder when it is set */
        if (custom && spread && custom->preferenceOrder) {
            found = custom->preferenceOrder(ring, key, keyLen, replicas,
                                            maxReplicas);
        } else if (custom && custom->locate) {
            found = custom->locate(ring, key, keyLen, replicas, maxReplicas);
        }
        break;
    }

    const clusterResult result =
        clusterRingSelect(ring, replicas, &found, maxReplicas, replicaCount,
                          rules, ruleCount, &out->spreadMet);
    if (result != CLUSTER_OK) {
        clusterPlacementFree(out);
        return result;
    }

    /* Copy results to the placement */
    out->primary = replicas[0];
    out->healthyCount = 0;
    for (uint32_t i = 0; i < found; i++) {
        clusterPlacementAddReplica(out, replicas[i]);
        if (replicas[i]->state == CLUSTER_NODE_UP) {
            out->healthyCount++;
        }
    }

    return CLUSTER_OK;
}

/* Turn a strategy's `*found` candidates (preference order, room for
 * maxReplicas) into the replica set in place: with rules, healthy nodes the
 * strategy left out join after its ranking (ring order) and the selection
 * spreads replicaCount of them; without, the candidates are the set.
 * CLUSTER_ERR_NO_NODES with no candidate, CLUSTER_ERR_AFFINITY when a
 * required rule cannot be met. */
static clusterResult clusterRingSelect(const clusterRing *ring,
                                       const clusterNode **replicas,
                                       uint32_t *foundOut, uint32_t maxReplicas,
                                       uint32_t replicaCount,
                                       const clusterAffinityRule *rules,
                                       uint8_t ruleCount, bool *spreadMet) {
    uint32_t found = *foundOut;
    const bool spread = rules && ruleCount > 0;
    *spreadMet = true;
    if (found == 0) {
        return CLUSTER_ERR_NO_NODES;
    }

    /* Spread the replicas as the rules ask (in place: the selection keeps
     * preference order, so it never overtakes the candidates it reads).
     * A strategy may rank only some nodes (Jump's probes collide), so the
     * healthy nodes it left out follow its ranking, in ring order. */
    if (spread && found < maxReplicas) {
        seenTracker listed;
        seenTrackerInit(&listed, ring->nodeCount);
        for (uint32_t c = 0; c < found; c++) {
            seenTrackerSet(&listed, replicas[c]->arrayIndex);
        }
        for (uint32_t i = 0; i < ring->nodeCount && found < maxReplicas; i++) {
            const clusterNode *node = ring->nodeArray[i];
            if (node && node->state == CLUSTER_NODE_UP &&
                !seenTrackerTest(&listed, i)) {
                replicas[found++] = node;
            }
        }
        seenTrackerFree(&listed);
    }

    if (spread) {
        bool requiredMet = true;
        found =
            clusterSelectReplicas(replicas, found, replicaCount, rules,
                                  ruleCount, replicas, &requiredMet, spreadMet);
        if (!requiredMet) {
            return CLUSTER_ERR_AFFINITY;
        }
    }

    *foundOut = found;
    return CLUSTER_OK;
}

/* clusterRingPlace, counted in locateOps; one call in
 * CLUSTER_LATENCY_SAMPLE_EVERY is also timed into the latency stats */
static clusterResult
clusterRingLocateWith(const clusterRing *ring, clusterStrategyType strategy,
                      const void *key, uint32_t keyLen, uint32_t replicaCount,
                      const clusterAffinityRule *rules, uint8_t ruleCount,
                      clusterPlacement *out) {
    if (!ring || !key || keyLen == 0 || !out) {
        return CLUSTER_ERR;
    }

    clusterRing *stats = (clusterRing *)ring;
    const uint64_t op =
        atomic_fetch_add_explicit(&stats->locateOps, 1, memory_order_relaxed);
    if (CLUSTER_LIKELY(op % CLUSTER_LATENCY_SAMPLE_EVERY != 0)) {
        return clusterRingPlace(ring, strategy, key, keyLen, replicaCount,
                                rules, ruleCount, out);
    }

    const uint64_t start = clusterGetTimeNs();
    const clusterResult result = clusterRingPlace(
        ring, strategy, key, keyLen, replicaCount, rules, ruleCount, out);
    clusterLatencyRecord(&stats->locateLatency, clusterGetTimeNs() - start);
    return result;
}

clusterResult clusterRingLocate(const clusterRing *ring, const void *key,
                                uint32_t keyLen, clusterPlacement *out) {
    clusterPlacementInit(out);
    if (!ring) {
        return CLUSTER_ERR;
    }
    return clusterRingLocateWith(
        ring, ring->strategyType, key, keyLen, ring->defaultQuorum.replicaCount,
        ring->affinityRules, ring->affinityRuleCount, out);
}

/* The bytes a boxed key places by: a string's content (embedded or not)
 * and a container's bytes, a number's value bytes (16 for a 128-bit
 * value), and the type alone for true/false/null, so equal keys always
 * place alike.  False for a key with no readable value (an offset box, a
 * 128-bit box without its value). */
static bool clusterKeyBytes(const databox *key, uint8_t scratch[16],
                            const void **data, uint32_t *len) {
    switch (key->type) {
    case DATABOX_BYTES:
    case DATABOX_BYTES_EMBED:
        *data = databoxBytes(key);
        *len = (uint32_t)key->len;
        return true;
    case DATABOX_CONTAINER_FLEX_MAP:
    case DATABOX_CONTAINER_FLEX_LIST:
    case DATABOX_CONTAINER_FLEX_SET:
    case DATABOX_CONTAINER_FLEX_TUPLE:
        *data = key->data.bytes.start;
        *len = (uint32_t)key->len;
        return true;
    case DATABOX_SIGNED_64:
    case DATABOX_UNSIGNED_64:
        memcpy(scratch, &key->data.u64, sizeof(key->data.u64));
        *len = sizeof(key->data.u64);
        break;
    case DATABOX_FLOAT_32:
        memcpy(scratch, &key->data.f32, sizeof(key->data.f32));
        *len = sizeof(key->data.f32);
        break;
    case DATABOX_DOUBLE_64:
        memcpy(scratch, &key->data.d64, sizeof(key->data.d64));
        *len = sizeof(key->data.d64);
        break;
    case DATABOX_SIGNED_128:
    case DATABOX_UNSIGNED_128:
        if (!key->data.u128) {
            return false;
        }
        memcpy(scratch, key->data.u128, 16);
        *len = 16;
        break;
    case DATABOX_BYTES_OFFSET:
        return false;
    default:
        scratch[0] = (uint8_t)key->type;
        *len = 1;
        break;
    }

    *data = scratch;
    return true;
}

clusterResult clusterRingLocateBox(const clusterRing *ring, const databox *key,
                                   clusterPlacement *out) {
    uint8_t scratch[16];
    const void *keyData = NULL;
    uint32_t keyLen = 0;
    clusterPlacementInit(out);
    if (!key || !clusterKeyBytes(key, scratch, &keyData, &keyLen)) {
        return CLUSTER_ERR;
    }

    return clusterRingLocate(ring, keyData, keyLen, out);
}

clusterResult clusterRingLocateKeyspace(const clusterRing *ring,
                                        const clusterKeySpace *ks,
                                        const void *key, uint32_t keyLen,
                                        clusterPlacement *out) {
    clusterPlacementInit(out);
    if (!ring || !ks) {
        return CLUSTER_ERR;
    }

    /* The keyspace's strategy, replica count and rules, else the ring's */
    const uint32_t replicaCount = ks->quorum.replicaCount
                                      ? ks->quorum.replicaCount
                                      : ring->defaultQuorum.replicaCount;
    const bool ownRules = ks->rules && ks->ruleCount > 0;
    clusterResult result = clusterRingLocateWith(
        ring, clusterRingResolveStrategy(ring, ks->strategy), key, keyLen,
        replicaCount, ownRules ? ks->rules : ring->affinityRules,
        ownRules ? ks->ruleCount : ring->affinityRuleCount, out);
    if (result == CLUSTER_OK) {
        out->keySpace = ks;
    }
    return result;
}

clusterResult clusterRingLocateBulk(const clusterRing *ring,
                                    const databox *keys, uint32_t keyCount,
                                    clusterPlacement *placements) {
    if (CLUSTER_UNLIKELY(!placements)) {
        return CLUSTER_ERR;
    }
    if (CLUSTER_UNLIKELY(!ring || !keys || keyCount == 0)) {
        for (uint32_t i = 0; i < keyCount; i++) {
            clusterPlacementInit(&placements[i]);
        }
        return CLUSTER_ERR;
    }

    /* Each key is placed exactly as clusterRingLocateBox places it; the
     * next string key's bytes are prefetched while this one is placed.
     * Every key is attempted: a failed key's placement is left empty and
     * the last failure is returned. */
    clusterResult finalResult = CLUSTER_OK;
    for (uint32_t i = 0; i < keyCount; i++) {
        if (i + 1 < keyCount && keys[i + 1].type == DATABOX_BYTES) {
            CLUSTER_PREFETCH(keys[i + 1].data.bytes.start);
        }

        const clusterResult result =
            clusterRingLocateBox(ring, &keys[i], &placements[i]);
        if (CLUSTER_UNLIKELY(result != CLUSTER_OK)) {
            finalResult = result;
        }
    }

    return finalResult;
}

/* ====================================================================
 * Routing Decision APIs
 * ==================================================================== */

clusterResult clusterRingPlanWrite(const clusterRing *ring, const void *key,
                                   uint32_t keyLen, const clusterQuorum *quorum,
                                   clusterWriteSet *out) {
    clusterWriteSetInit(out);
    if (!ring || !key || !out) {
        return CLUSTER_ERR;
    }

    const clusterQuorum *q = quorum ? quorum : &ring->defaultQuorum;

    /* Get placement */
    clusterPlacement placement;
    clusterResult result = clusterRingLocateWith(
        ring, ring->strategyType, key, keyLen,
        q->replicaCount ? q->replicaCount : ring->defaultQuorum.replicaCount,
        ring->affinityRules, ring->affinityRuleCount, &placement);
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
    atomic_fetch_add_explicit(&((clusterRing *)ring)->writeOps, 1,
                              memory_order_relaxed);

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
    clusterReadSetInit(out);
    if (!ring || !key || !out) {
        return CLUSTER_ERR;
    }

    const clusterQuorum *q = quorum ? quorum : &ring->defaultQuorum;

    /* Get placement */
    clusterPlacement placement;
    clusterResult result = clusterRingLocateWith(
        ring, ring->strategyType, key, keyLen,
        q->replicaCount ? q->replicaCount : ring->defaultQuorum.replicaCount,
        ring->affinityRules, ring->affinityRuleCount, &placement);
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
    atomic_fetch_add_explicit(&((clusterRing *)ring)->readOps, 1,
                              memory_order_relaxed);

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

    /* The least loaded healthy replica with a load report (the first of
     * equals, so ties keep preference order) */
    for (size_t i = 0; i < replicaCount; i++) {
        const clusterNode *node = clusterPlacementGetReplica(placement, i);
        if (node && node->state == CLUSTER_NODE_UP && node->loadReported &&
            (!*selected ||
             node->lastLoad.cpuUsage < (*selected)->lastLoad.cpuUsage)) {
            *selected = node;
        }
    }
    if (*selected) {
        return CLUSTER_OK;
    }

    /* Else the first healthy replica */
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

static uint64_t getLocationValue(const clusterLocation *loc,
                                 clusterTopologyLevel level) {
    switch (level) {
    case CLUSTER_LEVEL_NODE:
        return loc->nodeId;
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
    cleanerName(zfree, U64) uint64_t *values = zcalloc(count, sizeof(uint64_t));
    if (!values) {
        return false; /* Allocation failure - fail constraint check */
    }

    bool result = true;

    for (uint8_t r = 0; r < ruleCount && result; r++) {
        const clusterAffinityRule *rule = &rules[r];

        /* Count distinct values at this level */
        uint32_t uniqueCount = 0;

        for (uint32_t i = 0; i < count; i++) {
            const uint64_t val =
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

    if (!clusterRingValidStrategyType((uint64_t)config->strategy) ||
        !clusterRingValidQuorum(&config->quorum) ||
        !clusterRingValidRules(config->rules, config->ruleCount)) {
        return CLUSTER_ERR_INVALID_CONFIG;
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
        /* @own: container; free=clusterRingFree */
        clusterKeySpace **newArr =
            zrealloc(ring->keySpaces, newCap * sizeof(clusterKeySpace *));
        if (!newArr) {
            return CLUSTER_ERR_ALLOC_FAILED;
        }
        ring->keySpaces = newArr;
        ring->keySpaceCapacity = newCap;
    }

    /* Create keyspace */
    /* @own: container; free=clusterRingFree */
    clusterKeySpace *ks = zcalloc(1, sizeof(*ks));
    if (!ks) {
        return CLUSTER_ERR_ALLOC_FAILED;
    }

    /* Convert const char * to mdsc internally */
    /* @own: container; free=mdscfree */
    ks->name = mdscnew(config->name);
    ks->id = ring->keySpaceCount;
    ks->quorum = config->quorum;

    /* The keyspace places with its strategy: build that strategy's state
     * if no one used it yet */
    const clusterResult used = clusterRingUseStrategy(
        ring, clusterRingResolveStrategy(ring, config->strategy));
    if (used != CLUSTER_OK) {
        mdscfree(ks->name);
        zfree(ks);
        return used;
    }
    ks->strategy = config->strategy;

    /* Copy rules */
    if (config->rules && config->ruleCount > 0) {
        /* @own: container; free=clusterRingFree */
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

    /* Its strategy loses a user (the last frees the strategy's state) */
    clusterRingReleaseStrategy(ring,
                               clusterRingResolveStrategy(ring, ks->strategy));

    /* Free keyspace */
    mdscfree(ks->name);
    zfree(ks->rules);
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

static void clusterRebalancePlanFree(clusterRebalancePlan *plan) {
    if (plan) {
        zfree(plan->moves);
        zfree(plan);
    }
}

clusterResult clusterRebalancePlanAddMove(clusterRebalancePlan *plan,
                                          const clusterRebalanceMove *move) {
    if (!plan || !move) {
        return CLUSTER_ERR;
    }
    if (plan->moveCount == plan->moveCapacity) {
        const uint32_t capacity =
            plan->moveCapacity ? plan->moveCapacity * 2 : 16;
        /* @own: container; free=clusterRebalancePlanFree */
        plan->moves = zrealloc(plan->moves, capacity * sizeof(*plan->moves));
        plan->moveCapacity = capacity;
    }
    clusterRebalanceMove *added = &plan->moves[plan->moveCount++];
    *added = *move;
    added->state = CLUSTER_MOVE_PENDING;
    return CLUSTER_OK;
}

/* A Ketama ring's placement per arc: arc i holds the hashes in
 * (points[i-1], points[i]] (arc 0 also those above the last point) and
 * places them on nodes[i * width ...] (counts[i] of them).  A placement
 * starts at the first vnode at or after the key's hash, so one walk per
 * distinct vnode point gives every arc's replica set. */
typedef struct clusterArcs {
    uint64_t *points;
    const clusterNode **nodes;
    uint32_t *counts;
    uint32_t count;
    uint32_t width;
} clusterArcs;

static void clusterArcsRelease_cleanup(clusterArcs *a) {
    zfree(a->points);
    zfree(a->nodes);
    zfree(a->counts);
}

/* The arcs of `r`'s own placement (its replica count and rules) */
static void clusterArcsBuild(const clusterRing *r, clusterArcs *a) {
    const clusterKetamaData *v = &r->vnodes;
    uint32_t replicaCount = r->defaultQuorum.replicaCount;
    if (replicaCount > 256) {
        replicaCount = 256;
    }
    const bool spread = r->affinityRules && r->affinityRuleCount > 0;
    const uint32_t maxReplicas = spread ? r->nodeCount : replicaCount;

    *a = (clusterArcs){.width = replicaCount ? replicaCount : 1};
    if (v->vnodeCount == 0 || maxReplicas == 0) {
        return;
    }

    /* @own: container; free=clusterArcsRelease */
    a->points = zcalloc(v->vnodeCount, sizeof(*a->points));
    /* @own: container; free=clusterArcsRelease */
    a->counts = zcalloc(v->vnodeCount, sizeof(*a->counts));
    /* @own: container; free=clusterArcsRelease */
    a->nodes = zcalloc((size_t)v->vnodeCount * a->width, sizeof(*a->nodes));
    cleanerName(zfree, NodePtrs) const clusterNode **scratch =
        zcalloc(maxReplicas, sizeof(*scratch));

    for (uint32_t i = 0; i < v->vnodeCount; i++) {
        if (i > 0 && v->vnodes[i].hashPoint == v->vnodes[i - 1].hashPoint) {
            continue; /* placements start at the first of equal points */
        }
        uint32_t found =
            clusterKetamaWalkFrom(r, i, scratch, maxReplicas, -1.0);
        bool spreadMet;
        if (clusterRingSelect(r, scratch, &found, maxReplicas, replicaCount,
                              r->affinityRules, r->affinityRuleCount,
                              &spreadMet) != CLUSTER_OK) {
            found = 0; /* no placement: the arc has no holders */
        }
        a->points[a->count] = v->vnodes[i].hashPoint;
        a->counts[a->count] = found;
        memcpy(&a->nodes[(size_t)a->count * a->width], scratch,
               found * sizeof(*scratch));
        a->count++;
    }
}

/* Called for each hash range [lo, hi] over which both rings' placements
 * are constant: `from` places it on oldNodes, `to` on newNodes */
typedef void clusterArcSegmentFn(void *ctx, uint64_t lo, uint64_t hi,
                                 const clusterNode **oldNodes,
                                 uint32_t oldCount,
                                 const clusterNode **newNodes,
                                 uint32_t newCount);

/* Visit the hash space in segments bounded by every point of either arc
 * table; a segment ending at u lies in the arc of each table's first
 * point at or after u (wrapping to arc 0) */
static void clusterArcsOverlay(const clusterArcs *a, const clusterArcs *b,
                               clusterArcSegmentFn *fn, void *ctx) {
    if (a->count == 0 && b->count == 0) {
        return;
    }

    uint32_t ia = 0;
    uint32_t ib = 0;
    uint64_t lo = 0;
    bool done = false;
    while (!done && (ia < a->count || ib < b->count)) {
        uint64_t hi = UINT64_MAX;
        if (ia < a->count && a->points[ia] < hi) {
            hi = a->points[ia];
        }
        if (ib < b->count && b->points[ib] < hi) {
            hi = b->points[ib];
        }

        const uint32_t arcA = ia < a->count ? ia : 0;
        const uint32_t arcB = ib < b->count ? ib : 0;
        fn(ctx, lo, hi, a->count ? &a->nodes[(size_t)arcA * a->width] : NULL,
           a->count ? a->counts[arcA] : 0,
           b->count ? &b->nodes[(size_t)arcB * b->width] : NULL,
           b->count ? b->counts[arcB] : 0);

        ia += ia < a->count && a->points[ia] == hi;
        ib += ib < b->count && b->points[ib] == hi;
        done = hi == UINT64_MAX;
        lo = hi + 1;
    }

    /* Past the last point: arc 0 of each */
    if (!done) {
        fn(ctx, lo, UINT64_MAX, a->count ? a->nodes : NULL,
           a->count ? a->counts[0] : 0, b->count ? b->nodes : NULL,
           b->count ? b->counts[0] : 0);
    }
}

typedef struct clusterPlanContext {
    const clusterRing *from;
    const clusterRing *to;
    clusterRebalancePlan *plan;
    unsigned __int128 *held; /* hash count each `from` node holds */
    uint32_t *open;          /* moves ending just before this segment */
    uint32_t openCount;
    uint32_t *next; /* moves ending at this segment */
    uint32_t nextCount;
} clusterPlanContext;

static bool clusterNodesHoldId(const clusterNode **nodes, uint32_t count,
                               uint64_t id) {
    for (uint32_t i = 0; i < count; i++) {
        if (nodes[i]->id == id) {
            return true;
        }
    }
    return false;
}

static void clusterPlanCountHeld(void *ctx, uint64_t lo, uint64_t hi,
                                 const clusterNode **oldNodes,
                                 uint32_t oldCount,
                                 const clusterNode **newNodes,
                                 uint32_t newCount) {
    (void)newNodes;
    (void)newCount;
    clusterPlanContext *c = ctx;
    for (uint32_t i = 0; i < oldCount; i++) {
        c->held[oldNodes[i]->arrayIndex] += (unsigned __int128)(hi - lo) + 1;
    }
}

static void clusterPlanSegment(void *ctx, uint64_t lo, uint64_t hi,
                               const clusterNode **oldNodes, uint32_t oldCount,
                               const clusterNode **newNodes,
                               uint32_t newCount) {
    clusterPlanContext *c = ctx;
    c->nextCount = 0;

    for (uint32_t t = 0; t < newCount && oldCount > 0; t++) {
        const uint64_t target = newNodes[t]->id;
        if (clusterNodesHoldId(oldNodes, oldCount, target)) {
            continue;
        }

        /* The first holder still serving: UP, or removed (decommission) */
        const clusterNode *source = oldNodes[0];
        for (uint32_t o = 0; o < oldCount; o++) {
            const clusterNode *now = clusterRingGetNode(c->to, oldNodes[o]->id);
            if (!now || now->state == CLUSTER_NODE_UP) {
                source = oldNodes[o];
                break;
            }
        }

        /* The source's bytes in proportion to the hashes it holds */
        const clusterNode *sourceNow = clusterRingGetNode(c->to, source->id);
        const uint64_t used =
            sourceNow ? sourceNow->usedBytes : source->usedBytes;
        const unsigned __int128 held = c->held[source->arrayIndex];
        const uint64_t estimate =
            held ? (uint64_t)((unsigned __int128)used *
                              ((unsigned __int128)(hi - lo) + 1) / held)
                 : 0;

        /* Extend this pair's move from the segment before, else add one */
        uint32_t index = UINT32_MAX;
        for (uint32_t m = 0; m < c->openCount; m++) {
            const clusterRebalanceMove *move = &c->plan->moves[c->open[m]];
            if (move->sourceNodeId == source->id &&
                move->targetNodeId == target) {
                index = c->open[m];
                break;
            }
        }
        if (index != UINT32_MAX) {
            c->plan->moves[index].rangeEnd = hi;
            c->plan->moves[index].estimatedBytes += estimate;
        } else {
            const clusterRebalanceMove move = {.rangeStart = lo,
                                               .rangeEnd = hi,
                                               .sourceNodeId = source->id,
                                               .targetNodeId = target,
                                               .estimatedBytes = estimate};
            index = c->plan->moveCount;
            clusterRebalancePlanAddMove(c->plan, &move);
        }
        c->next[c->nextCount++] = index;
    }

    uint32_t *swap = c->open;
    c->open = c->next;
    c->next = swap;
    c->openCount = c->nextCount;
}

/* Exact Ketama plan: overlay both rings' arc tables */
static void clusterPlanKetama(const clusterRing *from, const clusterRing *to,
                              clusterRebalancePlan *plan) {
    cleaner(clusterArcsRelease) clusterArcs before;
    cleaner(clusterArcsRelease) clusterArcs after;
    clusterArcsBuild(from, &before);
    clusterArcsBuild(to, &after);

    cleanerName(zfree, U32) uint32_t *open =
        zcalloc(after.width, sizeof(*open));
    cleanerName(zfree, U32) uint32_t *next =
        zcalloc(after.width, sizeof(*next));
    cleaner(zfree) void *held = zcalloc(from->nodeCount ? from->nodeCount : 1,
                                        sizeof(unsigned __int128));
    clusterPlanContext c = {.from = from,
                            .to = to,
                            .plan = plan,
                            .held = held,
                            .open = open,
                            .next = next};

    clusterArcsOverlay(&before, &after, clusterPlanCountHeld, &c);
    clusterArcsOverlay(&before, &after, clusterPlanSegment, &c);
}

clusterRing *clusterRingCopy(const clusterRing *ring) {
    if (!ring) {
        return NULL;
    }
    flex *f = clusterRingBuildSnapshotFlex(ring);
    /* @own: caller; free=clusterRingFree */
    clusterRing *copy = clusterRingDeserialize(f, flexBytes(f));
    flexFree(f);

    /* Custom hooks are code: the copy shares them, and their data stays
     * the original's to free */
    if (copy && ring->strategyType == CLUSTER_STRATEGY_CUSTOM) {
        copy->customStrategy = ring->customStrategy;
        copy->sharesCustomStrategy = true;
    }
    return copy;
}

clusterResult clusterRingPlanRebalance(clusterRing *ring,
                                       const clusterRing *from) {
    if (!ring || !from) {
        return CLUSTER_ERR;
    }

    cleaner(clusterRebalancePlanFree) clusterRebalancePlan *plan =
        zcalloc(1, sizeof(*plan));
    plan->createdAt = clusterGetTimeMs();

    const clusterStrategy *custom = ring->customStrategy;
    if (ring->strategyType == CLUSTER_STRATEGY_KETAMA) {
        if (from->strategyType != CLUSTER_STRATEGY_KETAMA ||
            from->hashSeed != ring->hashSeed) {
            return CLUSTER_ERR_INVALID_CONFIG;
        }
        clusterPlanKetama(from, ring, plan);
    } else if (ring->strategyType == CLUSTER_STRATEGY_CUSTOM && custom &&
               custom->planRebalance) {
        if (!custom->planRebalance(from, ring, plan)) {
            return CLUSTER_ERR;
        }
    } else {
        return CLUSTER_ERR_UNSUPPORTED;
    }

    for (uint32_t m = 0; m < plan->moveCount; m++) {
        plan->totalBytes += plan->moves[m].estimatedBytes;
    }

    clusterRebalancePlanFree(ring->rebalancePlan);
    ring->rebalancePlan = plan;
    plan = NULL;

    if (ring->rebalanceCallback) {
        ring->rebalanceCallback(ring, ring->rebalancePlan,
                                ring->rebalanceCallbackData);
    }
    return CLUSTER_OK;
}

void clusterRebalancePlanGetInfo(const clusterRebalancePlan *plan,
                                 clusterRebalanceInfo *info) {
    if (!info) {
        return;
    }
    *info = (clusterRebalanceInfo){0};
    if (plan) {
        *info = (clusterRebalanceInfo){.moveCount = plan->moveCount,
                                       .completedCount = plan->completedCount,
                                       .failedCount = plan->failedCount,
                                       .totalBytes = plan->totalBytes,
                                       .movedBytes = plan->movedBytes,
                                       .createdAtMs = plan->createdAt,
                                       .startedAtMs = plan->startedAt};
    }
}

/* The ring's move `moveIndex` when it is in `state`, else why not */
static clusterResult clusterRingMoveIn(clusterRing *ring, uint32_t moveIndex,
                                       clusterMoveState state,
                                       clusterRebalanceMove **move) {
    if (!ring || !ring->rebalancePlan) {
        return CLUSTER_ERR;
    }
    if (moveIndex >= ring->rebalancePlan->moveCount) {
        return CLUSTER_ERR_NOT_FOUND;
    }
    *move = &ring->rebalancePlan->moves[moveIndex];
    return (*move)->state == state ? CLUSTER_OK : CLUSTER_ERR_INVALID_STATE;
}

clusterResult clusterRingStartMove(clusterRing *ring, uint32_t moveIndex) {
    clusterRebalanceMove *move = NULL;
    clusterResult result =
        clusterRingMoveIn(ring, moveIndex, CLUSTER_MOVE_PENDING, &move);
    if (result == CLUSTER_ERR_INVALID_STATE &&
        move->state == CLUSTER_MOVE_FAILED) {
        ring->rebalancePlan->failedCount--;
        result = CLUSTER_OK;
    }
    if (result != CLUSTER_OK) {
        return result;
    }

    move->state = CLUSTER_MOVE_IN_PROGRESS;
    if (!ring->rebalancePlan->startedAt) {
        ring->rebalancePlan->startedAt = clusterGetTimeMs();
    }
    return CLUSTER_OK;
}

clusterResult clusterRingFailMove(clusterRing *ring, uint32_t moveIndex) {
    clusterRebalanceMove *move = NULL;
    const clusterResult result =
        clusterRingMoveIn(ring, moveIndex, CLUSTER_MOVE_IN_PROGRESS, &move);
    if (result != CLUSTER_OK) {
        return result;
    }

    move->state = CLUSTER_MOVE_FAILED;
    ring->rebalancePlan->failedCount++;
    if (ring->rebalanceCallback) {
        ring->rebalanceCallback(ring, ring->rebalancePlan,
                                ring->rebalanceCallbackData);
    }
    return CLUSTER_OK;
}

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
    clusterRebalanceMove *move = NULL;
    const clusterResult result =
        clusterRingMoveIn(ring, moveIndex, CLUSTER_MOVE_IN_PROGRESS, &move);
    if (result != CLUSTER_OK) {
        return result;
    }

    move->state = CLUSTER_MOVE_COMPLETED;
    ring->rebalancePlan->completedCount++;
    ring->rebalancePlan->movedBytes += move->estimatedBytes;
    ring->rebalanceMoves++;

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

    clusterRebalancePlanFree(ring->rebalancePlan);
    ring->rebalancePlan = NULL;
    return CLUSTER_OK;
}

/* ====================================================================
 * Health & Load Integration
 * ==================================================================== */

void clusterRingSetHealthProvider(clusterRing *ring,
                                  const clusterHealthProvider *provider) {
    if (!ring) {
        return;
    }

    const clusterHealthProvider old = ring->healthProvider;
    const bool hadProvider = ring->hasHealthProvider;
    ring->healthProvider = provider ? *provider : (clusterHealthProvider){0};
    ring->hasHealthProvider = provider != NULL;

    /* The replaced provider's data goes unless the new one keeps it */
    if (hadProvider && old.freeProvider &&
        !(provider && provider->providerData == old.providerData)) {
        old.freeProvider(old.providerData);
    }
}

const clusterNodeHealth *clusterNodeGetHealth(const clusterNode *node) {
    return node ? &node->lastHealth : NULL;
}

const clusterNodeLoad *clusterNodeGetLoad(const clusterNode *node) {
    return node ? &node->lastLoad : NULL;
}

clusterResult clusterRingPollHealth(clusterRing *ring) {
    if (!ring || !ring->hasHealthProvider) {
        return CLUSTER_ERR;
    }

    /* By id: a health update may change the node's state */
    const clusterHealthProvider *p = &ring->healthProvider;
    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        const uint64_t id = ring->nodeArray[i]->id;
        if (p->checkHealth) {
            const clusterNodeHealth health =
                p->checkHealth(p->providerData, id);
            clusterRingUpdateNodeHealth(ring, id, &health);
        }
        if (p->getLoad) {
            const clusterNodeLoad load = p->getLoad(p->providerData, id);
            clusterRingUpdateNodeLoad(ring, id, &load);
        }
    }
    return CLUSTER_OK;
}

clusterResult clusterRingUpdateNodeHealth(clusterRing *ring, uint64_t nodeId,
                                          const clusterNodeHealth *health) {
    if (!ring || !health) {
        return CLUSTER_ERR;
    }

    databox keybox = databoxNewUnsigned(nodeId);
    databox valbox;
    if (!multidictFind(ring->nodeById, &keybox, &valbox)) {
        return CLUSTER_ERR_NOT_FOUND;
    }

    clusterNode *node = (clusterNode *)valbox.data.ptr;
    node->lastHealth = *health;
    node->lastHealthCheck = clusterGetTimeMs();

    /* Consecutive unreachable reports suspect an UP node; a reachable one
     * clears the count and restores a SUSPECT node */
    if (!health->reachable) {
        node->failureCount++;
        if (node->state == CLUSTER_NODE_UP && node->failureCount >= 3) {
            clusterRingSetNodeState(ring, nodeId, CLUSTER_NODE_SUSPECT);
        }
    } else {
        node->failureCount = 0;
        if (node->state == CLUSTER_NODE_SUSPECT) {
            clusterRingSetNodeState(ring, nodeId, CLUSTER_NODE_UP);
        }
    }

    return CLUSTER_OK;
}

clusterResult clusterRingUpdateNodeLoad(clusterRing *ring, uint64_t nodeId,
                                        const clusterNodeLoad *load) {
    if (!ring || !load) {
        return CLUSTER_ERR;
    }

    databox keybox = databoxNewUnsigned(nodeId);
    databox valbox;
    if (!multidictFind(ring->nodeById, &keybox, &valbox)) {
        return CLUSTER_ERR_NOT_FOUND;
    }

    clusterNode *node = (clusterNode *)valbox.data.ptr;
    node->lastLoad = *load;
    node->lastLoadCheck = clusterGetTimeMs();
    node->loadReported = true;

    return CLUSTER_OK;
}

/* ====================================================================
 * Statistics
 * ==================================================================== */

void clusterRingGetStats(const clusterRing *ring, clusterRingStats *stats) {
    if (!stats) {
        return;
    }

    /* No ring reports an empty ring, so the caller never reads an
     * unwritten field. */
    if (!ring) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    stats->nodeCount = ring->nodeCount;
    stats->healthyNodes = ring->healthyNodeCount;
    stats->keySpaceCount = ring->keySpaceCount;

    /* Virtual node count (0 while no strategy in use places on vnodes) */
    stats->vnodeCount = ring->vnodes.vnodeCount;

    /* Copy operation stats */
    stats->locateOps =
        atomic_load_explicit(&ring->locateOps, memory_order_relaxed);
    stats->writeOps =
        atomic_load_explicit(&ring->writeOps, memory_order_relaxed);
    stats->readOps = atomic_load_explicit(&ring->readOps, memory_order_relaxed);
    stats->rebalanceMoves = ring->rebalanceMoves;
    clusterLatencySummary(&ring->locateLatency, &stats->avgLocateNs,
                          &stats->p99LocateNs, &stats->maxLocateNs);

    /* Calculate load distribution */
    if (ring->healthyNodeCount > 0 && clusterRingHasVnodes(ring)) {
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

    /* Every live strategy's state (zero for the ones not in use) */
    stats->memoryBytes += ring->vnodes.vnodeCapacity * sizeof(clusterVnode);
    stats->memoryBytes += ring->jump.bucketCount * sizeof(*ring->jump.nodes);
    stats->memoryBytes += ring->maglev.tableSize * sizeof(*ring->maglev.lookup);
    if (ring->rebalancePlan) {
        stats->memoryBytes +=
            sizeof(*ring->rebalancePlan) +
            ring->rebalancePlan->moveCapacity * sizeof(clusterRebalanceMove);
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
                                       uint64_t levelValue,
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
 * Serialization (one flex)
 * ====================================================================
 *
 * A snapshot is one flex; every field is a typed entry (unsigned
 * numbers, bytes for strings) and restore range-checks each one.
 *
 *   Restore gates:
 *     - flexValidateBufferDeep() bounds every entry before the walk.
 *     - Every entry is read with an explicit type and range check, and
 *       goes through the public constructors, which refuse what
 *       clusterRingNew / clusterRingAddKeySpace refuse.
 *     - The entry counts must match the recorded counts exactly.
 *
 *   Layout:
 *
 *     Header (17 entries):
 *       [ 0] ring name               (bytes, may be empty)
 *       [ 1] defaultQuorum.replicaCount      (unsigned, 0..255)
 *       [ 2] defaultQuorum.writeQuorum       (unsigned, 0..255)
 *       [ 3] defaultQuorum.writeSync         (unsigned, 0..255)
 *       [ 4] defaultQuorum.readQuorum        (unsigned, 0..255)
 *       [ 5] defaultQuorum.readRepairEnabled (unsigned, 0|1)
 *       [ 6] defaultQuorum.consistency       (unsigned, 0..5)
 *       [ 7] vnodeConfig.vnodeMultiplier  (unsigned, 0..UINT32_MAX)
 *       [ 8] vnodeConfig.minVnodesPerNode (unsigned, 0..UINT32_MAX)
 *       [ 9] vnodeConfig.maxVnodesPerNode (unsigned, 0..UINT32_MAX)
 *       [10] vnodeConfig.replicaPointSpread (unsigned, 0|1)
 *       [11] strategyType            (unsigned, 1..6: never DEFAULT)
 *       [12] hashSeed                (unsigned, 0..UINT32_MAX)
 *       [13] boundedLoadPercent      (unsigned, 100..UINT32_MAX)
 *       [14] ring affinity rule count RR (unsigned, 0..255)
 *       [15] nodeCount NN            (unsigned)
 *       [16] keySpaceCount KK        (unsigned)
 *
 *     Ring affinity rules (3 entries × RR): spreadLevel (unsigned,
 *     0..7), minSpread (unsigned, 0..255), required (unsigned, 0|1)
 *
 *     Per node (15 entries × NN nodes):
 *       [ 0] id                      (unsigned)
 *       [ 1] name                    (bytes)
 *       [ 2] address                 (bytes)
 *       [ 3] location.nodeId         (unsigned)
 *       [ 4] location.rackId         (unsigned, 0..UINT32_MAX)
 *       [ 5] location.cageId         (unsigned, 0..UINT32_MAX)
 *       [ 6] location.dcId           (unsigned, 0..UINT32_MAX)
 *       [ 7] location.azId           (unsigned, 0..UINT32_MAX)
 *       [ 8] location.regionId       (unsigned, 0..UINT32_MAX)
 *       [ 9] location.countryId      (unsigned, 0..UINT16_MAX)
 *       [10] location.continentId    (unsigned, 0..UINT8_MAX)
 *       [11] weight                  (unsigned, 0..UINT32_MAX)
 *       [12] capacityBytes           (unsigned)
 *       [13] state                   (unsigned, range-checked vs enum)
 *       [14] usedBytes               (unsigned)
 *
 *     Per keyspace (9 fixed entries + 3 entries per rule × KK keyspaces):
 *       [ 0] name                    (bytes)
 *       [ 1] quorum.replicaCount     (unsigned, 0..255)
 *       [ 2] quorum.writeQuorum      (unsigned, 0..255)
 *       [ 3] quorum.writeSync        (unsigned, 0..255)
 *       [ 4] quorum.readQuorum       (unsigned, 0..255)
 *       [ 5] quorum.readRepairEnabled (unsigned, 0|1)
 *       [ 6] quorum.consistency      (unsigned, 0..5)
 *       [ 7] strategy                (unsigned, 0..6; 0 = the ring's)
 *       [ 8] ruleCount RR            (unsigned, 0..255)
 *       then RR rules, 3 entries each as for the ring's rules
 *
 * Runtime state (health, load, stats, rebalance plans, callbacks, a
 * custom strategy's functions, the modification version) is not part
 * of a snapshot. */

#define CLUSTER_RING_HEADER_ENTRY_COUNT ((size_t)17)
#define CLUSTER_RING_NODE_ENTRY_COUNT ((size_t)15)
#define CLUSTER_RING_KEYSPACE_FIXED_ENTRY_COUNT ((size_t)9)
#define CLUSTER_RING_RULE_ENTRY_COUNT ((size_t)3)

/* The snapshot is written a section at a time: each section's entries are
 * gathered into a flexRow and appended exactly (flexRowAppendExact), so a
 * name or address that reads as a number stays bytes (crReadBytes accepts
 * only bytes).  A missing name is the empty string. */
static void crRowName(flexRow *r, const mdsc *name) {
    const size_t len = name ? mdsclen(name) : 0;
    flexRowBytes(r, len ? (const void *)name : "", len);
}

/* The ring's quorum (6 entries, in spec order). */
static void crRowQuorum(flexRow *r, const clusterQuorum *q) {
    flexRowUnsigned(r, (uint64_t)q->replicaCount);
    flexRowUnsigned(r, (uint64_t)q->writeQuorum);
    flexRowUnsigned(r, (uint64_t)q->writeSync);
    flexRowUnsigned(r, (uint64_t)q->readQuorum);
    flexRowUnsigned(r, (uint64_t)(q->readRepairEnabled ? 1u : 0u));
    flexRowUnsigned(r, (uint64_t)q->consistency);
}

/* Affinity rules, 3 entries each, one row per rule */
static void crAppendRules(flex **f, flexRow *row,
                          const clusterAffinityRule *rules, uint8_t count) {
    for (uint8_t r = 0; r < count; r++) {
        flexRowUnsigned(row, (uint64_t)rules[r].spreadLevel);
        flexRowUnsigned(row, (uint64_t)rules[r].minSpread);
        flexRowUnsigned(row, (uint64_t)(rules[r].required ? 1u : 0u));
        flexRowAppendExact(f, row);
    }
}

/* Build the canonical flex for this ring (caller frees via flexFree).
 * flex's encoding is byte-order independent. */
static flex *clusterRingBuildSnapshotFlex(const clusterRing *ring) {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();

    /* ---- Header (17 entries), then the ring's rules ---- */
    flexRow row = FLEX_ROW_INIT;
    crRowName(&row, ring->name);
    crRowQuorum(&row, &ring->defaultQuorum);
    flexRowUnsigned(&row, (uint64_t)ring->vnodeConfig.vnodeMultiplier);
    flexRowUnsigned(&row, (uint64_t)ring->vnodeConfig.minVnodesPerNode);
    flexRowUnsigned(&row, (uint64_t)ring->vnodeConfig.maxVnodesPerNode);
    flexRowUnsigned(&row,
                    (uint64_t)(ring->vnodeConfig.replicaPointSpread ? 1u : 0u));
    flexRowUnsigned(&row, (uint64_t)ring->strategyType);
    flexRowUnsigned(&row, (uint64_t)ring->hashSeed);
    flexRowUnsigned(&row, (uint64_t)ring->boundedLoadPercent);

    /* Count only non-NULL slots so the deserializer can iterate
     * sequentially without re-checking sparse positions. */
    uint64_t live_nodes = 0;
    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        if (ring->nodeArray[i]) {
            live_nodes++;
        }
    }
    uint64_t live_keyspaces = 0;
    for (uint32_t i = 0; i < ring->keySpaceCount; i++) {
        if (ring->keySpaces[i]) {
            live_keyspaces++;
        }
    }
    flexRowUnsigned(&row, (uint64_t)ring->affinityRuleCount);
    flexRowUnsigned(&row, live_nodes);
    flexRowUnsigned(&row, live_keyspaces);
    flexRowAppendExact(&f, &row);
    crAppendRules(&f, &row, ring->affinityRules, ring->affinityRuleCount);

    /* ---- Nodes (15 entries each) ---- */
    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        const clusterNode *node = ring->nodeArray[i];
        if (node == NULL) {
            continue;
        }
        flexRowUnsigned(&row, node->id);
        crRowName(&row, node->name);
        crRowName(&row, node->address);
        flexRowUnsigned(&row, node->location.nodeId);
        flexRowUnsigned(&row, (uint64_t)node->location.rackId);
        flexRowUnsigned(&row, (uint64_t)node->location.cageId);
        flexRowUnsigned(&row, (uint64_t)node->location.dcId);
        flexRowUnsigned(&row, (uint64_t)node->location.azId);
        flexRowUnsigned(&row, (uint64_t)node->location.regionId);
        flexRowUnsigned(&row, (uint64_t)node->location.countryId);
        flexRowUnsigned(&row, (uint64_t)node->location.continentId);
        flexRowUnsigned(&row, (uint64_t)node->weight);
        flexRowUnsigned(&row, node->capacityBytes);
        flexRowUnsigned(&row, (uint64_t)node->state);
        flexRowUnsigned(&row, node->usedBytes);
        flexRowAppendExact(&f, &row);
    }

    /* ---- Keyspaces (9 entries, then 3 per rule) ---- */
    for (uint32_t i = 0; i < ring->keySpaceCount; i++) {
        const clusterKeySpace *ks = ring->keySpaces[i];
        if (ks == NULL) {
            continue;
        }
        crRowName(&row, ks->name);
        crRowQuorum(&row, &ks->quorum);
        flexRowUnsigned(&row, (uint64_t)ks->strategy);
        flexRowUnsigned(&row, (uint64_t)ks->ruleCount);
        flexRowAppendExact(&f, &row);
        crAppendRules(&f, &row, ks->rules, ks->ruleCount);
    }

    return f;
}

size_t clusterRingSerializeSize(const clusterRing *ring) {
    if (!ring) {
        return 0;
    }
    flex *f = clusterRingBuildSnapshotFlex(ring);
    const size_t n = flexBytes(f);
    flexFree(f);
    return n;
}

size_t clusterRingSerialize(const clusterRing *ring, void *buf, size_t bufLen) {
    if (!ring || !buf) {
        return 0;
    }
    flex *f = clusterRingBuildSnapshotFlex(ring);
    const size_t need = flexBytes(f);
    if (bufLen < need) {
        flexFree(f);
        return 0;
    }
    memcpy(buf, f, need);
    flexFree(f);
    return need;
}

/* ---- Restore helpers (range-checked, NULL-safe). --------------------- */

/* Read an unsigned 64-bit entry in [lo, hi] and step past it.  On any
 * error, returns false and leaves *out unchanged. */
static bool crReadUnsignedRange(const flex *f, flexEntry **fe, uint64_t lo,
                                uint64_t hi, uint64_t *out) {
    if (*fe == NULL) {
        return false;
    }
    databoxBig box;
    flexGetByTypeBig(*fe, &box);
    if (box.type != DATABOX_UNSIGNED_64) {
        return false;
    }
    const uint64_t v = box.data.u64;
    if (v < lo || v > hi) {
        return false;
    }
    *out = v;
    *fe = flexNext(f, *fe);
    return true;
}

/* Read a bytes entry as a NUL-terminated heap string (caller frees with
 * zfree) and step past it.  A string holding a NUL is refused: names
 * and addresses are C strings. */
static bool crReadString(const flex *f, flexEntry **fe, char **out) {
    if (*fe == NULL) {
        return false;
    }
    databoxBig box;
    flexGetByTypeBig(*fe, &box);
    if (!DATABOX_IS_BYTES(&box)) {
        return false;
    }
    const uint8_t *bytes = databoxBytes(&box);
    if (memchr(bytes, '\0', box.len) != NULL) {
        return false;
    }
    /* @own: caller; free=zfree */
    char *s = zmalloc(box.len + 1);
    memcpy(s, bytes, box.len);
    s[box.len] = '\0';
    *out = s;
    *fe = flexNext(f, *fe);
    return true;
}

/* Read `count` affinity rules (3 entries each) into a heap array (caller
 * frees with zfree; NULL when count is 0). */
static bool crReadRules(const flex *f, flexEntry **fe, uint64_t count,
                        clusterAffinityRule **out) {
    *out = NULL;
    if (count == 0) {
        return true;
    }
    /* @own: caller; free=zfree */
    clusterAffinityRule *rules = zcalloc((size_t)count, sizeof(*rules));
    for (uint64_t r = 0; r < count; r++) {
        uint64_t level = 0;
        uint64_t minSpread = 0;
        uint64_t required = 0;
        if (!crReadUnsignedRange(f, fe, 0, CLUSTER_LEVEL_COUNT - 1, &level) ||
            !crReadUnsignedRange(f, fe, 0, UINT8_MAX, &minSpread) ||
            !crReadUnsignedRange(f, fe, 0, 1, &required)) {
            zfree(rules);
            return false;
        }
        rules[r].spreadLevel = (clusterTopologyLevel)level;
        rules[r].minSpread = (uint8_t)minSpread;
        rules[r].required = required != 0;
    }
    *out = rules;
    return true;
}

/* Read a 6-entry quorum block in spec order. */
static bool crReadQuorum(const flex *f, flexEntry **fe, clusterQuorum *out) {
    uint64_t v = 0;
    if (!crReadUnsignedRange(f, fe, 0, UINT8_MAX, &v)) {
        return false;
    }
    out->replicaCount = (uint8_t)v;
    if (!crReadUnsignedRange(f, fe, 0, UINT8_MAX, &v)) {
        return false;
    }
    out->writeQuorum = (uint8_t)v;
    if (!crReadUnsignedRange(f, fe, 0, UINT8_MAX, &v)) {
        return false;
    }
    out->writeSync = (uint8_t)v;
    if (!crReadUnsignedRange(f, fe, 0, UINT8_MAX, &v)) {
        return false;
    }
    out->readQuorum = (uint8_t)v;
    if (!crReadUnsignedRange(f, fe, 0, 1, &v)) {
        return false;
    }
    out->readRepairEnabled = (v != 0);
    if (!crReadUnsignedRange(f, fe, 0, UINT64_MAX, &v)) {
        return false;
    }
    if (!clusterRingValidConsistencyLevel(v)) {
        return false;
    }
    out->consistency = (clusterConsistencyLevel)v;
    return true;
}

clusterRing *clusterRingDeserialize(const void *buf, size_t bufLen) {
    if (!buf || !flexValidateBufferDeep((const uint8_t *)buf, bufLen)) {
        return NULL;
    }

    const flex *f = (const flex *)buf;
    const size_t total = flexCount(f);
    if (total < CLUSTER_RING_HEADER_ENTRY_COUNT) {
        return NULL;
    }

    flexEntry *fe = flexHead(f);

    /* ---- Header section ---- */
    uint64_t v = 0;
    clusterRingConfig config = {0};

    /* [0] ring name */
    cleanerName(zfree, Chars) char *name = NULL;
    if (!crReadString(f, &fe, &name)) {
        return NULL;
    }
    config.name = name;

    /* [1-6] defaultQuorum */
    if (!crReadQuorum(f, &fe, &config.defaultQuorum)) {
        return NULL;
    }

    /* [7-10] vnodeConfig */
    if (!crReadUnsignedRange(f, &fe, 0, UINT32_MAX, &v)) {
        return NULL;
    }
    config.vnodes.vnodeMultiplier = (uint32_t)v;
    if (!crReadUnsignedRange(f, &fe, 0, UINT32_MAX, &v)) {
        return NULL;
    }
    config.vnodes.minVnodesPerNode = (uint32_t)v;
    if (!crReadUnsignedRange(f, &fe, 0, UINT32_MAX, &v)) {
        return NULL;
    }
    config.vnodes.maxVnodesPerNode = (uint32_t)v;
    if (!crReadUnsignedRange(f, &fe, 0, 1, &v)) {
        return NULL;
    }
    config.vnodes.replicaPointSpread = (v != 0);

    /* [11] strategy (a ring's is never DEFAULT) */
    if (!crReadUnsignedRange(f, &fe, CLUSTER_STRATEGY_KETAMA,
                             CLUSTER_STRATEGY_CUSTOM, &v)) {
        return NULL;
    }
    config.strategyType = (clusterStrategyType)v;

    /* [12] hash seed */
    if (!crReadUnsignedRange(f, &fe, 0, UINT32_MAX, &v)) {
        return NULL;
    }
    config.hashSeed = (uint32_t)v;

    /* [13] bounded-load percent */
    if (!crReadUnsignedRange(f, &fe, 100, UINT32_MAX, &v)) {
        return NULL;
    }
    config.boundedLoadPercent = (uint32_t)v;

    /* [14-16] ring rule, node and keyspace counts */
    uint64_t ringRuleCount = 0;
    uint64_t nodeCount = 0;
    uint64_t ksCount = 0;
    if (!crReadUnsignedRange(f, &fe, 0, UINT8_MAX, &ringRuleCount) ||
        !crReadUnsignedRange(f, &fe, 0, UINT32_MAX, &nodeCount) ||
        !crReadUnsignedRange(f, &fe, 0, UINT32_MAX, &ksCount)) {
        return NULL;
    }

    /* The recorded counts must fit the entries present (keyspace rules
     * come on top), so no count can size an allocation past the input */
    const uint64_t neededMin =
        CLUSTER_RING_HEADER_ENTRY_COUNT +
        ringRuleCount * CLUSTER_RING_RULE_ENTRY_COUNT +
        nodeCount * CLUSTER_RING_NODE_ENTRY_COUNT +
        ksCount * CLUSTER_RING_KEYSPACE_FIXED_ENTRY_COUNT;
    if (neededMin > total) {
        return NULL;
    }

    cleanerName(zfree, Rules) clusterAffinityRule *ringRules = NULL;
    if (!crReadRules(f, &fe, ringRuleCount, &ringRules)) {
        return NULL;
    }
    config.affinityRules = ringRules;
    config.affinityRuleCount = (uint8_t)ringRuleCount;
    config.expectedNodeCount = (uint32_t)nodeCount;

    /* Create ring (refuses what clusterRingNew refuses). */
    cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);
    if (!ring) {
        return NULL;
    }

    /* ---- Nodes ---- */
    for (uint64_t i = 0; i < nodeCount; i++) {
        clusterNodeConfig nodeCfg = {0};

        /* [0] id */
        if (!crReadUnsignedRange(f, &fe, 0, UINT64_MAX, &v)) {
            return NULL;
        }
        nodeCfg.id = v;

        /* [1] name, [2] address */
        cleanerName(zfree, Chars) char *nodeName = NULL;
        cleanerName(zfree, Chars) char *address = NULL;
        if (!crReadString(f, &fe, &nodeName) ||
            !crReadString(f, &fe, &address)) {
            return NULL;
        }
        nodeCfg.name = nodeName;
        nodeCfg.address = address;

        /* [3-10] location fields */
        uint64_t loc[8];
        static const uint64_t locMax[8] = {UINT64_MAX, UINT32_MAX, UINT32_MAX,
                                           UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                           UINT16_MAX, UINT8_MAX};
        for (size_t l = 0; l < 8; l++) {
            if (!crReadUnsignedRange(f, &fe, 0, locMax[l], &loc[l])) {
                return NULL;
            }
        }
        nodeCfg.location.nodeId = loc[0];
        nodeCfg.location.rackId = (uint32_t)loc[1];
        nodeCfg.location.cageId = (uint32_t)loc[2];
        nodeCfg.location.dcId = (uint32_t)loc[3];
        nodeCfg.location.azId = (uint32_t)loc[4];
        nodeCfg.location.regionId = (uint32_t)loc[5];
        nodeCfg.location.countryId = (uint16_t)loc[6];
        nodeCfg.location.continentId = (uint8_t)loc[7];

        /* [11] weight (a live node's weight is never 0) */
        if (!crReadUnsignedRange(f, &fe, 1, UINT32_MAX, &v)) {
            return NULL;
        }
        nodeCfg.weight = (uint32_t)v;

        /* [12] capacityBytes */
        if (!crReadUnsignedRange(f, &fe, 0, UINT64_MAX, &v)) {
            return NULL;
        }
        nodeCfg.capacityBytes = v;

        /* [13] state */
        if (!crReadUnsignedRange(f, &fe, 0, CLUSTER_NODE_MAINTENANCE, &v)) {
            return NULL;
        }
        nodeCfg.initialState = (clusterNodeState)v;

        /* [14] usedBytes (applied post-add since AddNode does not
         * accept it through the config). */
        uint64_t usedBytes = 0;
        if (!crReadUnsignedRange(f, &fe, 0, UINT64_MAX, &usedBytes)) {
            return NULL;
        }

        /* A repeated id is refused (CLUSTER_ERR_EXISTS); placement is
         * built once, after the last node */
        if (clusterRingAddNodeInternal(ring, &nodeCfg, false) != CLUSTER_OK) {
            return NULL;
        }

        /* The node just added is the array's last */
        clusterNode *added = ring->nodeArray[ring->nodeCount - 1];
        clusterBoundedAccount(ring, added, -1);
        added->usedBytes = usedBytes;
        clusterBoundedAccount(ring, added, 1);
    }

    clusterRingPlaceNodes(ring);

    /* ---- Keyspaces ---- */
    for (uint64_t i = 0; i < ksCount; i++) {
        clusterKeySpaceConfig ksCfg = {0};

        /* [0] name */
        cleanerName(zfree, Chars) char *ksName = NULL;
        if (!crReadString(f, &fe, &ksName)) {
            return NULL;
        }
        ksCfg.name = ksName;

        /* [1-6] quorum */
        if (!crReadQuorum(f, &fe, &ksCfg.quorum)) {
            return NULL;
        }

        /* [7] strategy */
        if (!crReadUnsignedRange(f, &fe, 0, CLUSTER_STRATEGY_CUSTOM, &v)) {
            return NULL;
        }
        ksCfg.strategy = (clusterStrategyType)v;

        /* [8] ruleCount, then the rules (AddKeySpace copies them) */
        uint64_t ruleCount = 0;
        if (!crReadUnsignedRange(f, &fe, 0, UINT8_MAX, &ruleCount)) {
            return NULL;
        }
        cleanerName(zfree, Rules) clusterAffinityRule *rules = NULL;
        if (!crReadRules(f, &fe, ruleCount, &rules)) {
            return NULL;
        }
        ksCfg.rules = rules;
        ksCfg.ruleCount = (uint8_t)ruleCount;

        /* A repeated name is refused (CLUSTER_ERR_EXISTS) */
        if (clusterRingAddKeySpace(ring, &ksCfg, NULL) != CLUSTER_OK) {
            return NULL;
        }
    }

    /* Entries past the recorded counts: malformed */
    if (fe != NULL) {
        return NULL;
    }

    /* Success: disarm the cleaner - caller owns the ring now */
    clusterRing *result = ring;
    ring = NULL;
    return result;
}

uint64_t clusterRingGetVersion(const clusterRing *ring) {
    return ring ? ring->version : 0;
}

/* ====================================================================
 * persistOps descriptor — integrity wrap via the persist framework
 * ==================================================================== */

static uint8_t *clusterRingPersistSnapshot(const void *structure, size_t *len) {
    /* The snapshot flex is the persisted bytes; the caller frees it */
    flex *f = clusterRingBuildSnapshotFlex((const clusterRing *)structure);
    *len = flexBytes(f);
    return (uint8_t *)f;
}

static void *clusterRingPersistRestore(const uint8_t *data, size_t len) {
    return clusterRingDeserialize(data, len);
}

/* Allocate a fresh, empty ring for the persist framework's WAL-replay
 * path (which constructs an empty target via newEmpty when the WAL has
 * no leading snapshot). Returns a default-configured ring with no
 * nodes; caller frees via .free (clusterRingFree). */
static void *crPersistNewEmpty(void) {
    /* @own: caller; free=clusterRingFree */
    return clusterRingNewDefault();
}

static size_t clusterRingPersistCount(const void *structure) {
    return clusterRingNodeCount((const clusterRing *)structure);
}

static size_t clusterRingPersistEstimateSize(const void *structure) {
    return clusterRingSerializeSize((const clusterRing *)structure);
}

static void clusterRingPersistFree(void *structure) {
    clusterRingFree((clusterRing *)structure);
}

/* Validate hook — runs after restore to re-check invariants that an
 * adversarial wire input could violate even after passing field-level
 * range checks.  The descriptor framework gates restored structures on
 * this returning true before they reach application code. */
static bool clusterRingPersistValidate(const void *structure) {
    const clusterRing *ring = (const clusterRing *)structure;
    if (ring == NULL) {
        return false;
    }

    /* The ring's configuration passes the checks clusterRingNew makes */
    if (!clusterRingValidStrategyType((uint64_t)ring->strategyType) ||
        ring->strategyType == CLUSTER_STRATEGY_DEFAULT ||
        !clusterRingValidQuorum(&ring->defaultQuorum) ||
        !clusterRingValidRules(ring->affinityRules, ring->affinityRuleCount)) {
        return false;
    }

    /* Per-node invariants: a known state and a unique id */
    for (uint32_t i = 0; i < ring->nodeCount; i++) {
        const clusterNode *a = ring->nodeArray[i];
        if (a == NULL) {
            continue;
        }
        if (!clusterRingValidNodeState((uint64_t)a->state)) {
            return false;
        }
        for (uint32_t j = i + 1; j < ring->nodeCount; j++) {
            const clusterNode *b = ring->nodeArray[j];
            if (b == NULL) {
                continue;
            }
            if (a->id == b->id) {
                return false;
            }
        }
    }

    /* Per-keyspace invariants. */
    for (uint32_t i = 0; i < ring->keySpaceCount; i++) {
        const clusterKeySpace *ks = ring->keySpaces[i];
        if (ks == NULL) {
            continue;
        }
        if (!clusterRingValidStrategyType((uint64_t)ks->strategy) ||
            !clusterRingValidQuorum(&ks->quorum) ||
            !clusterRingValidRules(ks->rules, ks->ruleCount)) {
            return false;
        }
    }

    return true;
}

const persistOps persistOpsClusterRing = {
    .type = PERSIST_TYPE_CLUSTER_RING,
    .name = "clusterRing",
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = clusterRingPersistSnapshot,
    .restore = clusterRingPersistRestore,
    .newEmpty = crPersistNewEmpty,
    .count = clusterRingPersistCount,
    .estimateSize = clusterRingPersistEstimateSize,
    .encodeOp = NULL,
    .applyOp = NULL,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
    .validate = clusterRingPersistValidate,
    .free = clusterRingPersistFree,
};

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

    if (clusterRingHasVnodes(ring)) {
        printf("  VNodes: %u\n", ring->vnodes.vnodeCount);
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
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();
        if (!ring) {
            ERRR("Failed to create ring");
        } else {
            if (clusterRingNodeCount(ring) != 0) {
                ERRR("Expected 0 nodes");
            }
        }
    }

    TEST("Ring creation with custom config") {
        clusterRingConfig config = {
            .name = "test-ring",
            .strategyType = CLUSTER_STRATEGY_KETAMA,
            .defaultQuorum = CLUSTER_QUORUM_BALANCED,
            .hashSeed = 12345,
        };
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);
        if (!ring) {
            ERRR("Failed to create ring with custom config");
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
            cleaner(clusterRingFree) clusterRing *ring =
                clusterRingNew(&config);
            if (!ring) {
                ERR("Failed to create ring with strategy %zu", i);
                err++;
            }
        }
    }

    return err;
}

static int testNodeManagement(void) {
    int err = 0;

    cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();
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

    TEST("nodeIdToIndex reverse-index invariant after compaction") {
        /* The removal above compacts nodeArray and reindexes survivors, so
         * each node's cached arrayIndex must still equal its real position and
         * the O(1) nodeIdToIndex must agree. Guards the reverse-index fix
         * across the add/remove/compaction lifecycle. */
        for (uint32_t i = 0; i < ring->nodeCount; i++) {
            const clusterNode *n = ring->nodeArray[i];
            if (!n) {
                ERR("nodeArray[%u] NULL after compaction", i);
                err++;
                continue;
            }
            if (n->arrayIndex != i) {
                ERR("arrayIndex mismatch: id %" PRIu64 " slot %u arrayIndex %u",
                    n->id, i, n->arrayIndex);
                err++;
            }
            if (nodeIdToIndex(ring, n->id) != (int32_t)i) {
                ERR("nodeIdToIndex(%" PRIu64 ") != slot %u", n->id, i);
                err++;
            }
        }
        if (nodeIdToIndex(ring, 0xDEADBEEFULL) != -1) {
            ERRR("nodeIdToIndex of unknown id must be -1");
            err++;
        }
    }

    return err;
}

static int testPlacement(void) {
    int err = 0;

    cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
            cleaner(clusterRingFree) clusterRing *ring =
                clusterRingNew(&config);

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
        }
    }

    return err;
}

static int testKeyspaces(void) {
    int err = 0;

    cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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

    return err;
}

static int testSerialization(void) {
    int err = 0;

    cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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

        cleaner(zfree) void *buf = zmalloc(size);
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
        }
    }

    TEST("Serialization keeps names that spell numbers as their bytes") {
        /* Names and addresses are strings even when they read as numbers;
         * the snapshot must store them exactly, or restore rejects them. */
        cleaner(clusterRingFree) clusterRing *numeric = clusterRingNewDefault();
        const char *names[] = {"22", "007", "1.5"};
        const char *addrs[] = {"10", "-3", "1e3"};
        for (uint64_t i = 1; i <= 3; i++) {
            clusterNodeConfig nc = {
                .id = i,
                .name = names[i - 1],
                .address = addrs[i - 1],
                .weight = 100,
                .initialState = CLUSTER_NODE_UP,
            };
            clusterRingAddNode(numeric, &nc);
        }

        clusterKeySpaceConfig kc = {.name = "123",
                                    .quorum = CLUSTER_QUORUM_BALANCED};
        clusterRingAddKeySpace(numeric, &kc, NULL);

        const size_t need = clusterRingSerializeSize(numeric);
        cleaner(zfree) void *buf = zmalloc(need ? need : 1);
        const size_t written = clusterRingSerialize(numeric, buf, need);
        clusterRing *restored =
            written ? clusterRingDeserialize(buf, written) : NULL;
        if (!restored) {
            ERRR("a ring whose names spell numbers did not restore");
        } else {
            for (uint64_t i = 1; i <= 3; i++) {
                const clusterNode *n = clusterRingGetNode(restored, i);
                if (!n || strcmp(clusterNodeGetName(n), names[i - 1]) != 0 ||
                    strcmp(clusterNodeGetAddress(n), addrs[i - 1]) != 0) {
                    ERR("node %" PRIu64 " lost its name or address", i);
                }
            }

            if (!clusterRingGetKeySpace(restored, "123")) {
                ERRR("keyspace \"123\" did not restore");
            }

            clusterRingFree(restored);
        }
    }

    return err;
}

static int testDistribution(void) {
    int err = 0;

    cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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

    return err;
}

static int testNodeFailure(void) {
    int err = 0;

    cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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

    return err;
}

static int testStats(void) {
    int err = 0;

    cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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

    TEST("Stats of a NULL ring are all zero") {
        clusterRingStats stats;
        memset(&stats, 0xA5, sizeof(stats));
        clusterRingGetStats(NULL, &stats);

        const uint8_t *bytes = (const uint8_t *)&stats;
        bool allZero = true;
        for (size_t i = 0; i < sizeof(stats); i++) {
            allZero = allZero && bytes[i] == 0;
        }

        if (!allZero) {
            ERRR("NULL ring should report all-zero stats");
            err++;
        }
    }

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

    cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();
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

    cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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

    return err;
}

/* ====================================================================
 * Comprehensive Fuzzing Tests
 * ==================================================================== */

static int testFuzzNodeChurn(void) {
    int err = 0;

    TEST("FUZZ: Random node add/remove churn") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();
        uint64_t nextNodeId = 1;
        cleanerName(zfree, U64) uint64_t *activeNodes =
            zcalloc(1000, sizeof(uint64_t));
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
    }

    return err;
}

static int testFuzzPlacementConsistency(void) {
    int err = 0;

    TEST("FUZZ: Placement consistency across operations") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
        cleanerName(zfree, U64) uint64_t *primaryIds =
            zcalloc(numKeys, sizeof(uint64_t));

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
    }

    TEST("FUZZ: Minimal movement on node removal") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
        cleanerName(zfree, U64) uint64_t *beforePrimary =
            zcalloc(numKeys, sizeof(uint64_t));

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
            cleaner(clusterRingFree) clusterRing *ring =
                clusterRingNew(&config);

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
        }
    }

    return err;
}

static int testFuzzStateTransitions(void) {
    int err = 0;

    TEST("FUZZ: Random state transitions") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
            cleaner(clusterRingFree) clusterRing *ring =
                clusterRingNew(&config);

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
                continue;
            }

            cleaner(zfree) void *buf = zmalloc(size);
            size_t written = clusterRingSerialize(ring, buf, size);
            if (written == 0) {
                ERR("Trial %d: Serialize returned 0", trial);
                err++;
                continue;
            }

            /* Deserialize */
            clusterRing *restored = clusterRingDeserialize(buf, written);
            if (!restored) {
                ERR("Trial %d: Deserialize returned NULL", trial);
                err++;
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

            clusterRingFree(restored);
        }
    }

    return err;
}

static int testFuzzConcurrentModification(void) {
    int err = 0;

    TEST("FUZZ: Interleaved modifications and lookups") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();
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
    }

    return err;
}

static int testFuzzLargeScale(void) {
    int err = 0;

    TEST("FUZZ: Large scale - 500 nodes") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
        cleanerName(zfree, U32) uint32_t *distribution =
            zcalloc(501, sizeof(uint32_t));
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
    }

    TEST("FUZZ: Large scale - many replicas request") {
        clusterRingConfig config = {
            .name = "many-replicas",
            .strategyType = CLUSTER_STRATEGY_KETAMA,
            .defaultQuorum = {.replicaCount = 50},
        };
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);

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
            cleanerName(zfree, U64) uint64_t *seen =
                zcalloc(replicaCount, sizeof(uint64_t));
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

            clusterPlacementFree(&p);
        }
    }

    return err;
}

static int testFuzzEdgeCases(void) {
    int err = 0;

    TEST("FUZZ: Single node cluster") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
    }

    TEST("FUZZ: Empty key") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
    }

    TEST("FUZZ: Very long key") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

        for (uint64_t i = 1; i <= 5; i++) {
            clusterNodeConfig config = {
                .id = i, .weight = 100, .initialState = CLUSTER_NODE_UP};
            clusterRingAddNode(ring, &config);
        }

        cleanerName(zfree, Chars) char *longKey = zmalloc(10000);
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
    }

    TEST("FUZZ: Binary key data") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
    }

    TEST("FUZZ: Extreme weights") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);

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
    }

    TEST("BENCH: Rendezvous locate throughput") {
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_RENDEZVOUS,
            .hashSeed = 12345,
        };
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);

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
    }

    TEST("BENCH: Jump hash locate throughput") {
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_JUMP,
            .hashSeed = 12345,
        };
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);

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
    }

    TEST("BENCH: Maglev locate throughput") {
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_MAGLEV,
            .hashSeed = 12345,
        };
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);

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
    }

    TEST("BENCH: Bulk locate vs single locate") {
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_KETAMA,
            .hashSeed = 12345,
        };
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);

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
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
    }

    TEST("EDGE: Bitmap tracker with 65 nodes (switch to medium)") {
        /* Test boundary case: transition from small to medium bitmap */
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
    }

    TEST("EDGE: Bitmap tracker with 512 nodes") {
        /* Test boundary case for medium bitmap (512 nodes) */
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
    }

    TEST("EDGE: Bitmap tracker with 513 nodes (large allocation)") {
        /* Test boundary case: transition to heap-allocated bitmap */
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
    }

    TEST("EDGE: Heap top-k with k > n (Rendezvous)") {
        /* Request more replicas than nodes available */
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_RENDEZVOUS,
            .hashSeed = 12345,
        };
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);

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
    }

    TEST("EDGE: Direct node pointer after vnode add") {
        /* Verify nodePtr is correctly set in vnodes */
        clusterRingConfig config = {.strategyType = CLUSTER_STRATEGY_KETAMA};
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);

        clusterNodeConfig nodeConfig = {
            .id = 42, .weight = 100, .initialState = CLUSTER_NODE_UP};
        clusterRingAddNode(ring, &nodeConfig);

        /* Check that vnodes have valid nodePtr */
        const clusterKetamaData *data = &ring->vnodes;
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
    }

    TEST("EDGE: Prefetch with single node ring") {
        /* Edge case: prefetch shouldn't crash with minimal data */
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
    }

    TEST("EDGE: Maglev prefetch at table boundary") {
        /* Test Maglev table wraparound with prefetching */
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_MAGLEV,
            .hashSeed = 99999, /* Seed to likely hit near-end indices */
        };
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);

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
    }

    TEST("EDGE: Bulk locate with mixed key types") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();

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
    }

    TEST("EDGE: Very long key (>248 bytes)") {
        /* Test Rendezvous with key that exceeds buffer in HRW calculation */
        clusterRingConfig config = {
            .strategyType = CLUSTER_STRATEGY_RENDEZVOUS,
        };
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);

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
    }

    return err;
}

#define CLUSTER_RING_TEST_KEYS 2000u

/* Primary node id for test key `k` ("c<k>") */
static uint64_t clusterRingTestPrimary(const clusterRing *ring, uint32_t k) {
    char key[16];
    const int len = snprintf(key, sizeof(key), "c%u", k);
    clusterPlacement p;
    uint64_t id = 0;
    if (clusterRingLocate(ring, key, (uint32_t)len, &p) == CLUSTER_OK) {
        id = p.primary->id;
        clusterPlacementFree(&p);
    }
    return id;
}

/* Primary node id for test key `k` placed in keyspace `ks` */
static uint64_t clusterRingTestKeyspacePrimary(const clusterRing *ring,
                                               const clusterKeySpace *ks,
                                               uint32_t k) {
    char key[16];
    const int len = snprintf(key, sizeof(key), "c%u", k);
    clusterPlacement p;
    uint64_t id = 0;
    if (clusterRingLocateKeyspace(ring, ks, key, (uint32_t)len, &p) ==
        CLUSTER_OK) {
        id = p.primary->id;
        clusterPlacementFree(&p);
    }
    return id;
}

/* First write target and first read candidate for test key `k` */
static void clusterRingTestPlanned(const clusterRing *ring, uint32_t k,
                                   uint64_t *writeId, uint64_t *readId) {
    char key[16];
    const int len = snprintf(key, sizeof(key), "c%u", k);
    clusterWriteSet ws;
    clusterReadSet rs;
    *writeId = *readId = 0;
    if (clusterRingPlanWrite(ring, key, (uint32_t)len, NULL, &ws) ==
        CLUSTER_OK) {
        const clusterNode *n = clusterWriteSetGetTarget(&ws, 0);
        *writeId = n ? n->id : 0;
        clusterWriteSetFree(&ws);
    }
    if (clusterRingPlanRead(ring, key, (uint32_t)len, NULL, &rs) ==
        CLUSTER_OK) {
        const clusterNode *n = clusterReadSetGetCandidate(&rs, 0);
        *readId = n ? n->id : 0;
        clusterReadSetFree(&rs);
    }
}

/* A health provider over per-node reachability and cpu (ids 1..8) */
typedef struct clusterRingTestHealth {
    bool reachable[9];
    float cpu[9];
    uint32_t frees;
} clusterRingTestHealth;

static clusterNodeHealth clusterRingTestCheckHealth(void *data, uint64_t id) {
    const clusterRingTestHealth *h = data;
    return (clusterNodeHealth){.reachable = h->reachable[id]};
}

static clusterNodeLoad clusterRingTestGetLoad(void *data, uint64_t id) {
    const clusterRingTestHealth *h = data;
    return (clusterNodeLoad){.cpuUsage = h->cpu[id]};
}

static void clusterRingTestFreeHealth(void *data) {
    clusterRingTestHealth *h = data;
    h->frees++;
}

/* Check `to`'s plan against both rings' placements of test keys: each
 * node `to` places a key on that `from` did not gets exactly one move
 * covering the key's hash, from a node `from` placed it on; no other move
 * covers it.  Returns the violations. */
static uint32_t clusterRingTestCheckPlan(const clusterRing *from,
                                         const clusterRing *to) {
    const clusterRebalancePlan *plan = clusterRingGetRebalancePlan(to);
    const uint32_t moveCount = clusterRebalancePlanMoveCount(plan);
    uint32_t violations = 0;
    for (uint32_t k = 0; k < CLUSTER_RING_TEST_KEYS; k++) {
        char key[16];
        const int len = snprintf(key, sizeof(key), "c%u", k);
        clusterPlacement before;
        clusterPlacement after;
        clusterRingLocate(from, key, (uint32_t)len, &before);
        clusterRingLocate(to, key, (uint32_t)len, &after);
        const uint64_t hash = after.hashValue;

        uint32_t covering = 0;
        for (uint32_t m = 0; m < moveCount; m++) {
            const clusterRebalanceMove *move =
                clusterRebalancePlanGetMove(plan, m);
            if (hash < move->rangeStart || hash > move->rangeEnd) {
                continue;
            }
            covering++;
            bool sourceHeld = false;
            bool targetNew = false;
            bool targetHeld = false;
            for (size_t r = 0; r < clusterPlacementReplicaCount(&before); r++) {
                const uint64_t id = clusterPlacementGetReplica(&before, r)->id;
                sourceHeld |= id == move->sourceNodeId;
                targetHeld |= id == move->targetNodeId;
            }
            for (size_t r = 0; r < clusterPlacementReplicaCount(&after); r++) {
                targetNew |= clusterPlacementGetReplica(&after, r)->id ==
                             move->targetNodeId;
            }
            violations += !sourceHeld || !targetNew || targetHeld;
        }

        uint32_t gained = 0;
        for (size_t r = 0; r < clusterPlacementReplicaCount(&after); r++) {
            const uint64_t id = clusterPlacementGetReplica(&after, r)->id;
            bool held = false;
            for (size_t o = 0; o < clusterPlacementReplicaCount(&before); o++) {
                held |= clusterPlacementGetReplica(&before, o)->id == id;
            }
            gained += !held && clusterPlacementReplicaCount(&before) > 0;
        }
        violations += covering != gained;
        clusterPlacementFree(&before);
        clusterPlacementFree(&after);
    }
    return violations;
}

static void clusterRingTestCountPlan(clusterRing *ring,
                                     const clusterRebalancePlan *plan,
                                     void *userData) {
    (void)ring;
    (void)plan;
    (*(uint32_t *)userData)++;
}

/* A CUSTOM strategy: locate ranks nodes by ascending id, preferenceOrder
 * by descending id, and planRebalance plans one whole-ring move */
static uint32_t clusterRingTestRankNodes(const clusterRing *ring,
                                         const clusterNode **out,
                                         uint32_t maxNodes, bool descending) {
    uint32_t found = 0;
    const uint32_t count = clusterRingNodeCount(ring);
    for (uint32_t i = 0; i < count && found < maxNodes; i++) {
        const uint64_t id = descending ? count - i : i + 1;
        const clusterNode *node = clusterRingGetNode(ring, id);
        if (node && clusterNodeGetState(node) == CLUSTER_NODE_UP) {
            out[found++] = node;
        }
    }
    return found;
}

static uint32_t clusterRingTestAscending(const clusterRing *ring,
                                         const void *key, uint32_t keyLen,
                                         const clusterNode **out,
                                         uint32_t maxNodes) {
    (void)key;
    (void)keyLen;
    return clusterRingTestRankNodes(ring, out, maxNodes, false);
}

static uint32_t clusterRingTestDescending(const clusterRing *ring,
                                          const void *key, uint32_t keyLen,
                                          const clusterNode **out,
                                          uint32_t maxNodes) {
    (void)key;
    (void)keyLen;
    return clusterRingTestRankNodes(ring, out, maxNodes, true);
}

static bool clusterRingTestPlanWhole(const clusterRing *from,
                                     const clusterRing *to,
                                     clusterRebalancePlan *plan) {
    const clusterRebalanceMove move = {
        .rangeEnd = UINT64_MAX,
        .sourceNodeId = clusterRingNodeCount(from),
        .targetNodeId = clusterRingNodeCount(to),
    };
    return clusterRebalancePlanAddMove(plan, &move) == CLUSTER_OK;
}

static void clusterRingTestFreeStrategy(void *data) {
    (*(uint32_t *)data)++;
}

typedef struct clusterRingTestConcurrent {
    const clusterRing *ring;
    const clusterKeySpace *ks;
    const uint64_t *expect;
    const uint64_t *expectKs;
    uint32_t mismatches;
} clusterRingTestConcurrent;

/* Every placement kind for every test key, each against its expectation:
 * a placement, a keyspace placement, and the write and read plans (which
 * lead with the primary) */
static void *clusterRingTestPlaceAll(void *arg) {
    clusterRingTestConcurrent *c = arg;
    for (uint32_t k = 0; k < CLUSTER_RING_TEST_KEYS; k++) {
        uint64_t writeId;
        uint64_t readId;
        clusterRingTestPlanned(c->ring, k, &writeId, &readId);
        c->mismatches += clusterRingTestPrimary(c->ring, k) != c->expect[k];
        c->mismatches += writeId != c->expect[k];
        c->mismatches += readId != c->expect[k];
        c->mismatches +=
            clusterRingTestKeyspacePrimary(c->ring, c->ks, k) != c->expectKs[k];
    }
    return NULL;
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

    /* persistOps descriptor round-trip — proves the same ring shape
     * survives through the standard persist pipeline. */
    TEST("persistOps round-trip via persistOpsClusterRing") {
        clusterRingConfig config = {
            .name = "persistops-roundtrip",
            .strategyType = CLUSTER_STRATEGY_KETAMA,
        };
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);
        for (uint32_t i = 1u; i <= 5u; i++) {
            clusterNodeConfig nc = {
                .id = (uint64_t)i,
                .weight = 100u,
                .capacityBytes = 1024u * 1024u,
                .initialState = CLUSTER_NODE_UP,
            };
            clusterRingAddNode(ring, &nc);
        }
        size_t snap_len = 0;
        uint8_t *snap = persistOpsClusterRing.snapshot(ring, &snap_len);
        if (snap == NULL || snap_len == 0) {
            ERRR("persistOps snapshot returned empty");
        }
        if (persistOpsClusterRing.count(ring) != 5u) {
            ERRR("persistOps count != 5");
        }
        if (persistOpsClusterRing.estimateSize(ring) != snap_len) {
            ERRR("estimateSize must agree with snapshot length");
        }
        clusterRing *restored =
            (clusterRing *)persistOpsClusterRing.restore(snap, snap_len);
        if (restored == NULL) {
            ERRR("persistOps restore returned NULL");
        }
        if (clusterRingNodeCount(restored) != 5u) {
            ERR("Restored node count expected 5, got %u",
                clusterRingNodeCount(restored));
        }
        persistOpsClusterRing.free(restored);
        zfree(snap);
    }

    /* persistOps newEmpty must produce a fresh, empty ring suitable for
     * WAL-replay paths that have no leading snapshot.  The framework
     * documents NULL as "skip with diagnostic"; this descriptor must
     * supply a usable constructor. */
    TEST("persistOps newEmpty returns a fresh-empty ring") {
        if (persistOpsClusterRing.newEmpty == NULL) {
            ERRR("persistOpsClusterRing.newEmpty must not be NULL");
        } else {
            clusterRing *empty =
                (clusterRing *)persistOpsClusterRing.newEmpty();
            if (empty == NULL) {
                ERRR("newEmpty() returned NULL");
            } else {
                if (clusterRingNodeCount(empty) != 0u) {
                    ERR("newEmpty ring must have 0 nodes, got %u",
                        clusterRingNodeCount(empty));
                }
                if (persistOpsClusterRing.count(empty) != 0u) {
                    ERRR("newEmpty ring count via descriptor must be 0");
                }
                persistOpsClusterRing.free(empty);
            }
        }
    }

    TEST("configuration checks: vnode bounds, quorums, strategies, rules") {
        const clusterVnodeConfig badVnodes[] = {
            {.minVnodesPerNode = 20, .maxVnodesPerNode = 10},
            {.maxVnodesPerNode = CLUSTER_VNODES_PER_NODE_MAX + 1},
            {.minVnodesPerNode = MAX_VNODES_PER_NODE + 1},
        };
        for (size_t i = 0; i < sizeof(badVnodes) / sizeof(*badVnodes); i++) {
            const clusterRingConfig c = {.vnodes = badVnodes[i]};
            cleaner(clusterRingFree) clusterRing *r = clusterRingNew(&c);
            if (r) {
                ERR("vnode config %zu accepted", i);
            }
        }

        const clusterQuorum badQuorum = {.replicaCount = 2, .writeQuorum = 3};
        clusterAffinityRule badRule = {.spreadLevel = CLUSTER_LEVEL_COUNT};
        const clusterRingConfig badConfigs[] = {
            {.defaultQuorum = badQuorum},
            {.strategyType =
                 (clusterStrategyType)(CLUSTER_STRATEGY_CUSTOM + 1)},
            {.affinityRules = &badRule, .affinityRuleCount = 1},
        };
        for (size_t i = 0; i < sizeof(badConfigs) / sizeof(*badConfigs); i++) {
            cleaner(clusterRingFree) clusterRing *r =
                clusterRingNew(&badConfigs[i]);
            if (r) {
                ERR("ring config %zu accepted", i);
            }
        }

        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();
        const clusterKeySpaceConfig badKeyspaces[] = {
            {.name = "q", .quorum = badQuorum},
            {.name = "s",
             .strategy = (clusterStrategyType)(CLUSTER_STRATEGY_CUSTOM + 1)},
            {.name = "r", .rules = &badRule, .ruleCount = 1},
        };
        for (size_t i = 0; i < sizeof(badKeyspaces) / sizeof(*badKeyspaces);
             i++) {
            if (clusterRingAddKeySpace(ring, &badKeyspaces[i], NULL) !=
                CLUSTER_ERR_INVALID_CONFIG) {
                ERR("keyspace config %zu not refused", i);
            }
        }

        /* Weight 0 is the default weight, whether set at add or later */
        const clusterNodeConfig nc = {.id = 7, .weight = 300};
        clusterRingAddNode(ring, &nc);
        clusterRingSetNodeWeight(ring, 7, 0);
        if (clusterNodeGetWeight(clusterRingGetNode(ring, 7)) != 100) {
            ERRR("weight 0 did not set the default weight");
        }
    }

    /* Every strategy adds nodes, places keys, and places them the same
     * way after a snapshot round trip; the snapshot keeps the ring's
     * affinity rules and names of any length.  Bounded-load places on
     * its own sorted vnode ring. */
    /* A key places by its value: a short string embedded in its box
     * places as the same string by pointer, a float ignores the box's
     * unused bytes, and bulk placement equals one-at-a-time placement
     * (with every placement freed: no leak) */
    TEST("boxed keys place by value; bulk equals single placement") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();
        for (uint64_t id = 1; id <= 12; id++) {
            const clusterNodeConfig nc = {.id = id};
            clusterRingAddNode(ring, &nc);
        }

        const char text[] = "abc";
        const databox byPtr = databoxNewBytes(text, 3);
        const databox embedded = databoxNewBytesAllowEmbed(text, 3);
        databox f1 = {.type = DATABOX_FLOAT_32};
        databox f2 = {.type = DATABOX_FLOAT_32};
        memset(&f1.data, 0x00, sizeof(f1.data));
        memset(&f2.data, 0xFF, sizeof(f2.data));
        f1.data.f32 = 1.5f;
        f2.data.f32 = 1.5f;
        const databox *pairs[][2] = {{&byPtr, &embedded}, {&f1, &f2}};
        for (size_t i = 0; i < 2; i++) {
            clusterPlacement a;
            clusterPlacement b;
            clusterRingLocateBox(ring, pairs[i][0], &a);
            clusterRingLocateBox(ring, pairs[i][1], &b);
            if (a.hashValue != b.hashValue || a.primary != b.primary) {
                ERR("pair %zu placed differently", i);
            }
            clusterPlacementFree(&a);
            clusterPlacementFree(&b);
        }

        databox keys[10];
        char names[10][8];
        for (size_t i = 0; i < 10; i++) {
            snprintf(names[i], sizeof(names[i]), "k%zu", i);
            keys[i] = databoxNewBytesString(names[i]);
        }
        clusterPlacement bulk[10];
        if (clusterRingLocateBulk(ring, keys, 10, bulk) != CLUSTER_OK) {
            ERRR("bulk placement failed");
        }
        for (size_t i = 0; i < 10; i++) {
            clusterPlacement one;
            clusterRingLocateBox(ring, &keys[i], &one);
            if (one.primary != bulk[i].primary) {
                ERR("key %zu placed differently in bulk", i);
            }
            clusterPlacementFree(&one);
            clusterPlacementFree(&bulk[i]);
        }
    }

    /* Placement meets affinity rules: required spread is reached or the
     * placement is refused, soft spread is best effort and reported, the
     * primary stays the strategy's first choice, keyspaces bring their
     * own replica count and rules, and write plans size by their quorum */
    TEST("affinity rules and keyspace settings shape every placement") {
        for (clusterStrategyType s = CLUSTER_STRATEGY_KETAMA;
             s < CLUSTER_STRATEGY_CUSTOM; s++) {
            clusterAffinityRule racks = {.spreadLevel = CLUSTER_LEVEL_RACK,
                                         .minSpread = 3,
                                         .required = true};
            const clusterRingConfig config = {
                .strategyType = s,
                .defaultQuorum = {.replicaCount = 3},
                .affinityRules = &racks,
                .affinityRuleCount = 1,
            };
            cleaner(clusterRingFree) clusterRing *ring =
                clusterRingNew(&config);
            const clusterRingConfig plainConfig = {
                .strategyType = s, .defaultQuorum = {.replicaCount = 3}};
            cleaner(clusterRingFree) clusterRing *plain =
                clusterRingNew(&plainConfig);
            for (uint64_t id = 1; id <= 12; id++) {
                /* Racks 0-3, zones 0-2; nodes 1-4 share rack 1's zone */
                const clusterNodeConfig nc = {
                    .id = id,
                    .location = {.nodeId = id,
                                 .rackId = (uint32_t)(id % 4),
                                 .azId = (uint32_t)(id % 3)}};
                clusterRingAddNode(ring, &nc);
                clusterRingAddNode(plain, &nc);
            }

            for (uint32_t k = 0; k < 200; k++) {
                char key[16];
                const int len = snprintf(key, sizeof(key), "key%u", k);
                clusterPlacement p;
                clusterPlacement q;
                if (clusterRingLocate(ring, key, (uint32_t)len, &p) !=
                    CLUSTER_OK) {
                    ERR("strategy %d key %u not placed", (int)s, k);
                    continue;
                }
                clusterRingLocate(plain, key, (uint32_t)len, &q);
                const size_t n = clusterPlacementReplicaCount(&p);
                uint64_t rackSeen = 0;
                for (size_t r = 0; r < n; r++) {
                    rackSeen |=
                        UINT64_C(1)
                        << clusterPlacementGetReplica(&p, r)->location.rackId;
                }
                if (n != 3 || __builtin_popcountll(rackSeen) != 3 ||
                    !p.spreadMet || p.primary->id != q.primary->id) {
                    ERR("strategy %d key %u: %zu replicas over %d racks, "
                        "primary %" PRIu64 " vs %" PRIu64,
                        (int)s, k, n, __builtin_popcountll(rackSeen),
                        p.primary->id, q.primary->id);
                    k = 200;
                }
                clusterPlacementFree(&p);
                clusterPlacementFree(&q);
            }

            /* Five racks cannot be had from four: required refuses, soft
             * places and reports */
            clusterAffinityRule five = {.spreadLevel = CLUSTER_LEVEL_RACK,
                                        .minSpread = 5,
                                        .required = true};
            clusterKeySpaceConfig hard = {.name = "hard",
                                          .quorum = {.replicaCount = 6},
                                          .rules = &five,
                                          .ruleCount = 1,
                                          .strategy = s};
            clusterKeySpace *hardKs = NULL;
            clusterRingAddKeySpace(ring, &hard, &hardKs);
            five.required = false;
            hard.name = "soft";
            clusterKeySpace *softKs = NULL;
            clusterRingAddKeySpace(ring, &hard, &softKs);
            clusterPlacement p;
            if (clusterRingLocateKeyspace(ring, hardKs, "k", 1, &p) !=
                CLUSTER_ERR_AFFINITY) {
                ERR("strategy %d: unmeetable required spread placed", (int)s);
            }
            if (clusterRingLocateKeyspace(ring, softKs, "k", 1, &p) !=
                    CLUSTER_OK ||
                p.spreadMet || clusterPlacementReplicaCount(&p) != 6 ||
                p.keySpace != softKs) {
                ERR("strategy %d: soft spread not best-effort", (int)s);
            }
            clusterPlacementFree(&p);

            /* A keyspace without rules uses the ring's; its replica count
             * is its own */
            const clusterKeySpaceConfig two = {
                .name = "two", .quorum = {.replicaCount = 2}, .strategy = s};
            clusterKeySpace *twoKs = NULL;
            clusterRingAddKeySpace(ring, &two, &twoKs);
            if (clusterRingLocateKeyspace(ring, twoKs, "k", 1, &p) !=
                CLUSTER_ERR_AFFINITY) {
                ERR("strategy %d: ring rule (3 racks) met by 2 replicas",
                    (int)s);
            }

            /* Write plans size by their quorum */
            const clusterQuorum five_replicas = {.replicaCount = 5,
                                                 .writeQuorum = 3};
            clusterWriteSet ws;
            if (clusterRingPlanWrite(ring, "k", 1, &five_replicas, &ws) !=
                    CLUSTER_OK ||
                clusterWriteSetTargetCount(&ws) != 5) {
                ERR("strategy %d: write plan ignored its quorum", (int)s);
            }
            clusterWriteSetFree(&ws);
        }
    }

    /* Bounded-load passes over a node filled above boundedLoadPercent of
     * the mean fill (Ketama on the same nodes does not), takes it last,
     * and keeps its fill sum exact through usage, state and membership
     * changes */
    TEST("bounded-load passes over overfilled nodes") {
        const clusterRingConfig boundedConfig = {
            .strategyType = CLUSTER_STRATEGY_BOUNDED,
            .defaultQuorum = {.replicaCount = 8},
            .boundedLoadPercent = 150,
        };
        const clusterRingConfig ketamaConfig = {
            .strategyType = CLUSTER_STRATEGY_KETAMA,
            .defaultQuorum = {.replicaCount = 8},
        };
        cleaner(clusterRingFree) clusterRing *ring =
            clusterRingNew(&boundedConfig);
        cleaner(clusterRingFree) clusterRing *ketama =
            clusterRingNew(&ketamaConfig);
        for (uint64_t id = 1; id <= 8; id++) {
            const clusterNodeConfig nc = {.id = id, .capacityBytes = 1000};
            clusterRingAddNode(ring, &nc);
            clusterRingAddNode(ketama, &nc);
            clusterRingSetNodeUsedBytes(ring, id, 100);
        }
        clusterRingSetNodeUsedBytes(ring, 3, 900); /* 0.9 vs mean 0.2 */

        uint32_t ketamaPrimary3 = 0;
        for (uint32_t k = 0; k < 400; k++) {
            char key[16];
            const int len = snprintf(key, sizeof(key), "b%u", k);
            clusterPlacement p;
            clusterPlacement q;
            clusterRingLocate(ring, key, (uint32_t)len, &p);
            clusterRingLocate(ketama, key, (uint32_t)len, &q);
            ketamaPrimary3 += q.primary->id == 3;
            if (p.primary->id == 3 || clusterPlacementReplicaCount(&p) != 8 ||
                clusterPlacementGetReplica(&p, 7)->id != 3) {
                ERR("key %u: overfilled node 3 not passed over", k);
                k = 400;
            }
            clusterPlacementFree(&p);
            clusterPlacementFree(&q);
        }
        if (ketamaPrimary3 == 0) {
            ERRR("ketama never chose node 3: the test proves nothing");
        }

        /* The fill sum tracks every change exactly */
        clusterRingSetNodeState(ring, 5, CLUSTER_NODE_DOWN);
        clusterRingRemoveNode(ring, 6);
        clusterRingSetNodeUsedBytes(ring, 7, 400);
        double sum = 0;
        uint32_t nodes = 0;
        for (uint32_t i = 0; i < ring->nodeCount; i++) {
            const clusterNode *n = ring->nodeArray[i];
            if (n && n->state == CLUSTER_NODE_UP && n->capacityBytes) {
                sum += (double)n->usedBytes / (double)n->capacityBytes;
                nodes++;
            }
        }
        if (ring->fillNodes != nodes || ring->fillSum < sum - 1e-9 ||
            ring->fillSum > sum + 1e-9) {
            ERR("fill sum %f over %u, recomputed %f over %u", ring->fillSum,
                ring->fillNodes, sum, nodes);
        }
        if (clusterRingSetNodeUsedBytes(ring, 99, 1) != CLUSTER_ERR_NOT_FOUND) {
            ERRR("usage set on a node not in the ring");
        }

        /* The setting and the usage persist; below 100% is refused */
        size_t len = 0;
        uint8_t *snap = persistOpsClusterRing.snapshot(ring, &len);
        clusterRing *back = persistOpsClusterRing.restore(snap, len);
        if (!back || back->boundedLoadPercent != 150 ||
            clusterNodeGetUsedBytes(clusterRingGetNode(back, 3)) != 900 ||
            back->fillNodes != nodes) {
            ERRR("bounded-load state did not round-trip");
        }
        clusterRingFree(back);
        zfree(snap);
        const clusterRingConfig low = {.boundedLoadPercent = 99};
        cleaner(clusterRingFree) clusterRing *refused = clusterRingNew(&low);
        if (refused) {
            ERRR("a bound below 100 percent was accepted");
        }
    }

    /* A keyspace places with its own strategy exactly as a ring of that
     * strategy over the same nodes would, through node changes and a
     * snapshot round trip, for every ring/keyspace strategy pair; the
     * state a strategy keeps lives exactly while someone uses it */
    TEST("keyspace strategies place as a ring of their strategy would") {
        for (clusterStrategyType r = CLUSTER_STRATEGY_KETAMA;
             r < CLUSTER_STRATEGY_CUSTOM; r++) {
            for (clusterStrategyType s = CLUSTER_STRATEGY_DEFAULT;
                 s < CLUSTER_STRATEGY_CUSTOM; s++) {
                const clusterStrategyType eff =
                    s == CLUSTER_STRATEGY_DEFAULT ? r : s;
                const clusterRingConfig rc = {
                    .strategyType = r,
                    .defaultQuorum = {.replicaCount = 3},
                    .hashSeed = 7};
                const clusterRingConfig qc = {
                    .strategyType = eff,
                    .defaultQuorum = {.replicaCount = 3},
                    .hashSeed = 7};
                cleaner(clusterRingFree) clusterRing *ring =
                    clusterRingNew(&rc);
                cleaner(clusterRingFree) clusterRing *ref = clusterRingNew(&qc);
                clusterRing *both[2] = {ring, ref};

                /* The keyspace exists before most nodes: its strategy's
                 * state must follow every later change */
                for (uint64_t id = 1; id <= 3; id++) {
                    const clusterNodeConfig nc = {.id = id,
                                                  .capacityBytes = 1000};
                    clusterRingAddNode(ring, &nc);
                    clusterRingAddNode(ref, &nc);
                }
                const clusterKeySpaceConfig kc = {.name = "ks", .strategy = s};
                clusterKeySpace *ks = NULL;
                clusterRingAddKeySpace(ring, &kc, &ks);
                for (int b = 0; b < 2; b++) {
                    for (uint64_t id = 4; id <= 16; id++) {
                        const clusterNodeConfig nc = {
                            .id = id,
                            .weight = (uint32_t)(50 + id * 10),
                            .capacityBytes = 1000};
                        clusterRingAddNode(both[b], &nc);
                        clusterRingSetNodeUsedBytes(both[b], id, id * 40);
                    }
                    clusterRingRemoveNode(both[b], 5);
                    clusterRingSetNodeState(both[b], 7, CLUSTER_NODE_DOWN);
                    clusterRingSetNodeWeight(both[b], 9, 400);
                }

                for (int round = 0; round < 2; round++) {
                    for (uint32_t k = 0; k < 150; k++) {
                        char key[16];
                        const int len = snprintf(key, sizeof(key), "q%u", k);
                        clusterPlacement p;
                        clusterPlacement q;
                        clusterRingLocateKeyspace(ring, ks, key, (uint32_t)len,
                                                  &p);
                        clusterRingLocate(ref, key, (uint32_t)len, &q);
                        const size_t n = clusterPlacementReplicaCount(&p);
                        bool same = n == clusterPlacementReplicaCount(&q);
                        for (size_t i = 0; same && i < n; i++) {
                            same = clusterPlacementGetReplica(&p, i)->id ==
                                   clusterPlacementGetReplica(&q, i)->id;
                        }
                        clusterPlacementFree(&p);
                        clusterPlacementFree(&q);
                        if (!same) {
                            ERR("ring %d keyspace %d round %d: key %u placed "
                                "differently",
                                (int)r, (int)s, round, k);
                            k = 150;
                            round = 2;
                        }
                    }

                    /* Round two: the same through a snapshot round trip */
                    if (round == 0) {
                        size_t snapLen = 0;
                        uint8_t *snap =
                            persistOpsClusterRing.snapshot(ring, &snapLen);
                        clusterRing *back =
                            persistOpsClusterRing.restore(snap, snapLen);
                        zfree(snap);
                        if (!back) {
                            ERRR("restore refused the ring");
                            break;
                        }
                        clusterRingFree(ring);
                        ring = back;
                        ks = (clusterKeySpace *)clusterRingGetKeySpace(ring,
                                                                       "ks");
                    }
                }

                /* Removing the keyspace frees state only it used */
                clusterRingRemoveKeySpace(ring, "ks");
                const bool vnodesLive = r == CLUSTER_STRATEGY_KETAMA ||
                                        r == CLUSTER_STRATEGY_BOUNDED;
                if ((ring->vnodes.vnodes != NULL) != vnodesLive ||
                    (ring->jump.nodes != NULL) !=
                        (r == CLUSTER_STRATEGY_JUMP) ||
                    (ring->maglev.lookup != NULL) !=
                        (r == CLUSTER_STRATEGY_MAGLEV)) {
                    ERR("ring %d keyspace %d: state outlived its users", (int)r,
                        (int)s);
                }
            }
        }
    }

    /* Placements may run concurrently: a Maglev table a node change left
     * stale is rebuilt once by whichever placement comes first, and every
     * thread sees the same placements a single thread does */
    TEST("latency buckets cover every value within a quarter of it") {
        /* Buckets are contiguous and in order, and each holds values
         * within 1/CLUSTER_LATENCY_SUB of its own */
        uint64_t next = 0;
        bool reachedMax = false;
        for (uint32_t b = 0; b < CLUSTER_LATENCY_BUCKETS; b++) {
            const uint64_t top = clusterLatencyBucketMax(b);
            if (clusterLatencyBucket(next) != b ||
                clusterLatencyBucket(top) != b) {
                ERR("bucket %u does not hold %" PRIu64 "..%" PRIu64, b, next,
                    top);
                break;
            }
            if (next >= CLUSTER_LATENCY_SUB &&
                (top - next) * CLUSTER_LATENCY_SUB > next) {
                ERR("bucket %u is too wide: %" PRIu64 "..%" PRIu64, b, next,
                    top);
            }
            if (top == UINT64_MAX) {
                reachedMax = true;
                break;
            }
            next = top + 1;
        }
        if (!reachedMax) {
            ERR("buckets end below UINT64_MAX at %" PRIu64, next);
        }

        /* 1..1000 (in reverse): mean 500, p99 the edge of the bucket
         * holding 990 (the 990th sample), capped at the maximum */
        clusterLatency latency = {0};
        clusterLatency *lat = &latency;
        uint64_t avg;
        uint64_t p99;
        uint64_t max;
        clusterLatencySummary(lat, &avg, &p99, &max);
        if (avg || p99 || max) {
            ERRR("an empty histogram reports latency");
        }
        for (uint64_t ns = 1000; ns >= 1; ns--) {
            clusterLatencyRecord(lat, ns);
        }
        clusterLatencySummary(lat, &avg, &p99, &max);
        const uint64_t edge =
            clusterLatencyBucketMax(clusterLatencyBucket(990));
        if (avg != 500 || max != 1000 || p99 != (edge < 1000 ? edge : 1000)) {
            ERR("summary avg %" PRIu64 " p99 %" PRIu64 " max %" PRIu64, avg,
                p99, max);
        }
    }

    TEST("ring stats report sampled placement latency") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();
        for (uint64_t id = 1; id <= 5; id++) {
            const clusterNodeConfig nc = {.id = id};
            clusterRingAddNode(ring, &nc);
        }
        clusterRingStats stats;
        clusterRingGetStats(ring, &stats);
        if (stats.avgLocateNs || stats.p99LocateNs || stats.maxLocateNs) {
            ERRR("latency before any placement");
        }
        for (uint32_t k = 0; k < 10 * CLUSTER_LATENCY_SAMPLE_EVERY; k++) {
            (void)clusterRingTestPrimary(ring, k);
        }
        clusterRingGetStats(ring, &stats);
        if (stats.locateOps != 10 * CLUSTER_LATENCY_SAMPLE_EVERY) {
            ERR("locateOps %" PRIu64, stats.locateOps);
        }
        if (stats.avgLocateNs == 0 || stats.avgLocateNs > stats.maxLocateNs ||
            stats.p99LocateNs == 0 || stats.p99LocateNs > stats.maxLocateNs) {
            ERR("latency avg %" PRIu64 " p99 %" PRIu64 " max %" PRIu64,
                stats.avgLocateNs, stats.p99LocateNs, stats.maxLocateNs);
        }
    }

    TEST("a health provider drives node state and read selection") {
        clusterRingTestHealth first = {0};
        clusterRingTestHealth second = {0};
        for (uint64_t id = 1; id <= 8; id++) {
            second.reachable[id] = true;
            second.cpu[id] = 0.9f - 0.1f * (float)id;
        }

        {
            const clusterRingConfig rc = {.strategyType =
                                              CLUSTER_STRATEGY_KETAMA,
                                          .defaultQuorum = {.replicaCount = 3}};
            cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&rc);
            for (uint64_t id = 1; id <= 8; id++) {
                const clusterNodeConfig nc = {.id = id};
                clusterRingAddNode(ring, &nc);
            }
            if (clusterRingPollHealth(ring) != CLUSTER_ERR) {
                ERRR("polled without a provider");
            }

            /* Without load reports the read goes to the first replica */
            clusterPlacement p;
            clusterRingLocate(ring, "k", 1, &p);
            const clusterNode *selected = NULL;
            clusterRingSelectReadNode(ring, &p, &selected);
            if (selected != p.primary) {
                ERRR("unreported loads should keep preference order");
            }

            /* Replacing the provider frees the replaced one's data once;
             * the stack-built provider is copied */
            {
                const clusterHealthProvider hp = {
                    .checkHealth = clusterRingTestCheckHealth,
                    .providerData = &first,
                    .freeProvider = clusterRingTestFreeHealth};
                clusterRingSetHealthProvider(ring, &hp);
            }
            {
                const clusterHealthProvider hp = {
                    .checkHealth = clusterRingTestCheckHealth,
                    .getLoad = clusterRingTestGetLoad,
                    .providerData = &second,
                    .freeProvider = clusterRingTestFreeHealth};
                clusterRingSetHealthProvider(ring, &hp);
            }
            if (first.frees != 1 || second.frees != 0) {
                ERR("frees after replace: %u %u", first.frees, second.frees);
            }

            /* Loads arrive by polling: the least loaded replica reads */
            clusterRingPollHealth(ring);
            const clusterNode *least = NULL;
            for (size_t r = 0; r < clusterPlacementReplicaCount(&p); r++) {
                const clusterNode *n = clusterPlacementGetReplica(&p, r);
                if (!least || second.cpu[n->id] < second.cpu[least->id]) {
                    least = n;
                }
            }
            clusterRingSelectReadNode(ring, &p, &selected);
            if (selected != least || clusterNodeGetLoad(selected)->cpuUsage !=
                                         second.cpu[least->id]) {
                ERRR("read did not go to the least loaded replica");
            }
            clusterPlacementFree(&p);

            /* Three consecutive unreachable polls suspect a node; a
             * reachable report between them restarts the count */
            const uint64_t victim = 5;
            const clusterNode *node = clusterRingGetNode(ring, victim);
            second.reachable[victim] = false;
            clusterRingPollHealth(ring);
            clusterRingPollHealth(ring);
            second.reachable[victim] = true;
            clusterRingPollHealth(ring);
            second.reachable[victim] = false;
            clusterRingPollHealth(ring);
            clusterRingPollHealth(ring);
            if (clusterNodeGetState(node) != CLUSTER_NODE_UP) {
                ERRR("non-consecutive failures suspected the node");
            }
            clusterRingPollHealth(ring);
            if (clusterNodeGetState(node) != CLUSTER_NODE_SUSPECT ||
                clusterNodeGetHealth(node)->reachable) {
                ERRR("three consecutive failures did not suspect the node");
            }
            second.reachable[victim] = true;
            clusterRingPollHealth(ring);
            if (clusterNodeGetState(node) != CLUSTER_NODE_UP) {
                ERRR("a reachable report did not restore the node");
            }
        }
        if (first.frees != 1 || second.frees != 1) {
            ERR("frees after ring free: %u %u", first.frees, second.frees);
        }
    }

    TEST("rebalance plans move exactly what placements gained") {
        clusterAffinityRule rackSpread = CLUSTER_AFFINITY_RACK_SPREAD;
        for (int withRules = 0; withRules <= 1; withRules++) {
            const clusterRingConfig rc = {
                .strategyType = CLUSTER_STRATEGY_KETAMA,
                .defaultQuorum = {.replicaCount = 3},
                .affinityRules = withRules ? &rackSpread : NULL,
                .affinityRuleCount = withRules ? 1 : 0};
            cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&rc);
            for (uint64_t id = 1; id <= 8; id++) {
                const clusterNodeConfig nc = {
                    .id = id,
                    .location = {.nodeId = id, .rackId = (uint32_t)(id % 4)},
                    .capacityBytes = 1u << 30};
                clusterRingAddNode(ring, &nc);
                clusterRingSetNodeUsedBytes(ring, id, id << 20);
            }
            uint32_t callbacks = 0;
            clusterRingSetRebalanceCallback(ring, clusterRingTestCountPlan,
                                            &callbacks);

            /* Each change, planned from the layout before it */
            for (int change = 0; change < 6; change++) {
                cleaner(clusterRingFree) clusterRing *settled =
                    clusterRingCopy(ring);
                switch (change) {
                case 0: /* nothing changed: nothing moves */
                    break;
                case 1: {
                    const clusterNodeConfig nc = {
                        .id = 9, .location = {.nodeId = 9, .rackId = 1}};
                    clusterRingAddNode(ring, &nc);
                    break;
                }
                case 2:
                    clusterRingRemoveNode(ring, 3);
                    break;
                case 3:
                    clusterRingSetNodeState(ring, 5, CLUSTER_NODE_DOWN);
                    break;
                case 4:
                    clusterRingSetNodeWeight(ring, 2, 400);
                    break;
                case 5:
                    clusterRingSetNodeState(ring, 5, CLUSTER_NODE_UP);
                    break;
                }
                if (clusterRingPlanRebalance(ring, settled) != CLUSTER_OK) {
                    ERR("rules %d change %d: plan refused", withRules, change);
                    continue;
                }
                const uint32_t moves = clusterRebalancePlanMoveCount(
                    clusterRingGetRebalancePlan(ring));
                if ((change == 0) != (moves == 0)) {
                    ERR("rules %d change %d: %u moves", withRules, change,
                        moves);
                }
                const uint32_t violations =
                    clusterRingTestCheckPlan(settled, ring);
                if (violations) {
                    ERR("rules %d change %d: %u violations", withRules, change,
                        violations);
                }

                /* A down node is not a source while another holder is up;
                 * a removed one may be (it still serves its data) */
                const clusterRebalancePlan *plan =
                    clusterRingGetRebalancePlan(ring);
                for (uint32_t m = 0; m < moves; m++) {
                    const clusterRebalanceMove *move =
                        clusterRebalancePlanGetMove(plan, m);
                    if (change == 3 && move->sourceNodeId == 5) {
                        ERRR("planned a copy from the down node");
                    }
                }
            }
            if (callbacks != 6) {
                ERR("rules %d: %u plan callbacks", withRules, callbacks);
            }
        }
    }

    TEST("rebalance estimates divide a removed node's bytes") {
        const clusterRingConfig rc = {.strategyType = CLUSTER_STRATEGY_KETAMA,
                                      .defaultQuorum = {.replicaCount = 1}};
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&rc);
        for (uint64_t id = 1; id <= 6; id++) {
            const clusterNodeConfig nc = {.id = id};
            clusterRingAddNode(ring, &nc);
        }
        const uint64_t used = (uint64_t)7 << 40;
        clusterRingSetNodeUsedBytes(ring, 4, used);
        cleaner(clusterRingFree) clusterRing *settled = clusterRingCopy(ring);
        clusterRingRemoveNode(ring, 4);
        clusterRingPlanRebalance(ring, settled);

        /* With one replica every range node 4 held moves off it, so its
         * moves' estimates add up to its bytes (less per-range rounding) */
        clusterRebalanceInfo info;
        clusterRebalancePlanGetInfo(clusterRingGetRebalancePlan(ring), &info);
        uint64_t fromRemoved = 0;
        for (uint32_t m = 0; m < info.moveCount; m++) {
            const clusterRebalanceMove *move = clusterRebalancePlanGetMove(
                clusterRingGetRebalancePlan(ring), m);
            if (move->sourceNodeId != 4) {
                ERRR("a move from a node that lost nothing");
            }
            fromRemoved += move->estimatedBytes;
        }
        if (fromRemoved > used || used - fromRemoved > info.moveCount * 1000u ||
            info.totalBytes != fromRemoved) {
            ERR("estimates %" PRIu64 " of %" PRIu64 " (total %" PRIu64 ")",
                fromRemoved, used, info.totalBytes);
        }
    }

    TEST("rebalance moves run start to finish") {
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNewDefault();
        for (uint64_t id = 1; id <= 4; id++) {
            const clusterNodeConfig nc = {.id = id};
            clusterRingAddNode(ring, &nc);
        }
        if (clusterRingStartMove(ring, 0) != CLUSTER_ERR ||
            clusterRingCancelRebalance(ring) != CLUSTER_ERR) {
            ERRR("moves without a plan");
        }
        cleaner(clusterRingFree) clusterRing *settled = clusterRingCopy(ring);
        const clusterNodeConfig nc = {.id = 5};
        clusterRingAddNode(ring, &nc);
        uint32_t callbacks = 0;
        clusterRingSetRebalanceCallback(ring, clusterRingTestCountPlan,
                                        &callbacks);
        clusterRingPlanRebalance(ring, settled);

        const clusterRebalancePlan *plan = clusterRingGetRebalancePlan(ring);
        const uint32_t moves = clusterRebalancePlanMoveCount(plan);
        clusterRebalanceInfo info;
        clusterRebalancePlanGetInfo(plan, &info);
        if (moves < 2 || info.moveCount != moves || info.startedAtMs != 0 ||
            clusterRebalancePlanProgress(plan) != 0.0f) {
            ERR("fresh plan: %u moves", moves);
        }
        if (clusterRingCompleteMove(ring, 0) != CLUSTER_ERR_INVALID_STATE ||
            clusterRingFailMove(ring, 0) != CLUSTER_ERR_INVALID_STATE ||
            clusterRingStartMove(ring, moves) != CLUSTER_ERR_NOT_FOUND) {
            ERRR("transitions out of order were accepted");
        }

        /* Move 0 fails once, then every move starts and completes */
        clusterRingStartMove(ring, 0);
        clusterRingFailMove(ring, 0);
        clusterRebalancePlanGetInfo(plan, &info);
        if (info.failedCount != 1 || info.startedAtMs == 0) {
            ERRR("failed move not counted");
        }
        for (uint32_t m = 0; m < moves; m++) {
            if (clusterRingStartMove(ring, m) != CLUSTER_OK ||
                clusterRingCompleteMove(ring, m) != CLUSTER_OK) {
                ERR("move %u did not run", m);
            }
        }
        clusterRebalancePlanGetInfo(plan, &info);
        clusterRingStats stats;
        clusterRingGetStats(ring, &stats);
        if (info.completedCount != moves || info.failedCount != 0 ||
            clusterRebalancePlanProgress(plan) != 1.0f ||
            stats.rebalanceMoves != moves || callbacks != 2 + moves ||
            clusterRingCompleteMove(ring, 0) != CLUSTER_ERR_INVALID_STATE) {
            ERR("finished plan: %u of %u, %u callbacks", info.completedCount,
                moves, callbacks);
        }
        if (clusterRingCancelRebalance(ring) != CLUSTER_OK ||
            clusterRingGetRebalancePlan(ring)) {
            ERRR("cancel kept the plan");
        }
    }

    TEST("rebalance refuses what it cannot plan by ranges") {
        cleaner(clusterRingFree) clusterRing *ketama = clusterRingNewDefault();
        for (clusterStrategyType s = CLUSTER_STRATEGY_JUMP;
             s <= CLUSTER_STRATEGY_BOUNDED; s++) {
            const clusterRingConfig rc = {.strategyType = s};
            cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&rc);
            if (clusterRingPlanRebalance(ring, ring) !=
                CLUSTER_ERR_UNSUPPORTED) {
                ERR("strategy %d planned by ranges", s);
            }
        }
        const clusterRingConfig seeded = {
            .strategyType = CLUSTER_STRATEGY_KETAMA, .hashSeed = 99};
        cleaner(clusterRingFree) clusterRing *other = clusterRingNew(&seeded);
        if (clusterRingPlanRebalance(other, ketama) !=
            CLUSTER_ERR_INVALID_CONFIG) {
            ERRR("planned across hash seeds");
        }
    }

    TEST("custom strategies rank, plan and copy through their hooks") {
        uint32_t frees = 0;
        clusterStrategy custom = {.name = "ranked",
                                  .locate = clusterRingTestAscending,
                                  .preferenceOrder = clusterRingTestDescending,
                                  .planRebalance = clusterRingTestPlanWhole,
                                  .strategyData = &frees,
                                  .freeStrategyData =
                                      clusterRingTestFreeStrategy};
        clusterAffinityRule nodeSpread = {.spreadLevel = CLUSTER_LEVEL_NODE,
                                          .minSpread = 1};
        const clusterRingConfig rc = {.strategyType = CLUSTER_STRATEGY_CUSTOM,
                                      .customStrategy = &custom,
                                      .defaultQuorum = {.replicaCount = 2}};
        {
            cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&rc);
            for (uint64_t id = 1; id <= 5; id++) {
                const clusterNodeConfig nc = {.id = id,
                                              .location = {.nodeId = id}};
                clusterRingAddNode(ring, &nc);
            }

            /* locate places the ring; preferenceOrder ranks for rules */
            clusterPlacement p;
            clusterRingLocate(ring, "k", 1, &p);
            const uint64_t plainPrimary = p.primary->id;
            clusterPlacementFree(&p);
            const clusterKeySpaceConfig kc = {
                .name = "spread", .rules = &nodeSpread, .ruleCount = 1};
            clusterKeySpace *ks = NULL;
            clusterRingAddKeySpace(ring, &kc, &ks);
            clusterRingLocateKeyspace(ring, ks, "k", 1, &p);
            const uint64_t rankedPrimary = p.primary->id;
            clusterPlacementFree(&p);
            if (plainPrimary != 1 || rankedPrimary != 5) {
                ERR("primaries %" PRIu64 " / %" PRIu64, plainPrimary,
                    rankedPrimary);
            }

            /* The copy shares the hooks and leaves their data alone */
            {
                cleaner(clusterRingFree) clusterRing *copy =
                    clusterRingCopy(ring);
                clusterRingLocate(copy, "k", 1, &p);
                if (!p.primary || p.primary->id != plainPrimary) {
                    ERRR("copy places differently");
                }
                clusterPlacementFree(&p);
                if (clusterRingPlanRebalance(ring, copy) != CLUSTER_OK ||
                    clusterRebalancePlanGetMove(
                        clusterRingGetRebalancePlan(ring), 0)
                            ->targetNodeId != 5) {
                    ERRR("custom plan not used");
                }
            }
            if (frees != 0) {
                ERRR("freeing the copy freed the hooks' data");
            }
        }
        if (frees != 1) {
            ERR("hooks' data freed %u times", frees);
        }
    }

    TEST("copies place as the original does") {
        for (clusterStrategyType s = CLUSTER_STRATEGY_KETAMA;
             s < CLUSTER_STRATEGY_CUSTOM; s++) {
            const clusterRingConfig rc = {.strategyType = s,
                                          .defaultQuorum = {.replicaCount = 2}};
            cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&rc);
            for (uint64_t id = 1; id <= 7; id++) {
                const clusterNodeConfig nc = {.id = id, .weight = 50 * id};
                clusterRingAddNode(ring, &nc);
            }
            clusterRingSetNodeState(ring, 3, CLUSTER_NODE_DOWN);
            cleaner(clusterRingFree) clusterRing *copy = clusterRingCopy(ring);
            for (uint32_t k = 0; k < 500; k++) {
                if (clusterRingTestPrimary(ring, k) !=
                    clusterRingTestPrimary(copy, k)) {
                    ERR("strategy %d: key %u placed differently", s, k);
                    break;
                }
            }
        }
    }

    TEST("concurrent placements on a stale Maglev table agree") {
        const clusterRingConfig mc = {.strategyType = CLUSTER_STRATEGY_MAGLEV,
                                      .defaultQuorum = {.replicaCount = 2}};
        cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&mc);
        for (uint64_t id = 1; id <= 9; id++) {
            const clusterNodeConfig nc = {.id = id};
            clusterRingAddNode(ring, &nc);
        }
        const clusterKeySpaceConfig kc = {.name = "jump",
                                          .quorum = {.replicaCount = 2},
                                          .strategy = CLUSTER_STRATEGY_JUMP};
        clusterKeySpace *ks = NULL;
        if (clusterRingAddKeySpace(ring, &kc, &ks) != CLUSTER_OK) {
            ERRR("jump keyspace refused");
        }

        clusterRingTestConcurrent ctx[4];
        uint64_t expect[CLUSTER_RING_TEST_KEYS];
        uint64_t expectKs[CLUSTER_RING_TEST_KEYS];
        for (uint32_t k = 0; k < CLUSTER_RING_TEST_KEYS; k++) {
            expect[k] = clusterRingTestPrimary(ring, k);
            expectKs[k] = clusterRingTestKeyspacePrimary(ring, ks, k);
        }
        clusterRingSetNodeState(ring, 4, CLUSTER_NODE_DOWN);
        clusterRingSetNodeState(ring, 4, CLUSTER_NODE_UP); /* stale, same set */

        pthread_t threads[4];
        for (int t = 0; t < 4; t++) {
            ctx[t] = (clusterRingTestConcurrent){
                .ring = ring, .ks = ks, .expect = expect, .expectKs = expectKs};
            pthread_create(&threads[t], NULL, clusterRingTestPlaceAll, &ctx[t]);
        }
        for (int t = 0; t < 4; t++) {
            pthread_join(threads[t], NULL);
            if (ctx[t].mismatches) {
                ERR("thread %d: %u placements differed", t, ctx[t].mismatches);
            }
        }
    }

    TEST("strategies place identically after a snapshot round trip") {
        char longName[6000];
        memset(longName, 'n', sizeof(longName) - 1);
        longName[sizeof(longName) - 1] = '\0';
        clusterAffinityRule rules[2] = {CLUSTER_AFFINITY_RACK_SPREAD,
                                        CLUSTER_AFFINITY_AZ_SPREAD};
        for (clusterStrategyType s = CLUSTER_STRATEGY_KETAMA;
             s < CLUSTER_STRATEGY_CUSTOM; s++) {
            const clusterRingConfig config = {
                .name = longName,
                .strategyType = s,
                .affinityRules = rules,
                .affinityRuleCount = 2,
                .hashSeed = 99,
            };
            cleaner(clusterRingFree) clusterRing *ring =
                clusterRingNew(&config);
            /* Nodes spread over 4 racks and 3 zones, so the ring's spread
             * rules can be met */
            const clusterNodeConfig batch[] = {
                {.id = 1, .weight = 50, .location = {.rackId = 1, .azId = 1}},
                {.id = 2, .location = {.rackId = 2, .azId = 2}},
                {.id = 3, .weight = 200, .location = {.rackId = 3}}};
            clusterRingAddNodes(ring, batch, 3);
            for (uint64_t id = 4; id <= 40; id++) {
                const clusterNodeConfig nc = {
                    .id = id,
                    .weight = (uint32_t)(id * 7),
                    .location = {.rackId = (uint32_t)(id % 4),
                                 .azId = (uint32_t)(id % 3)}};
                clusterRingAddNode(ring, &nc);
            }
            clusterRingRemoveNode(ring, 5);
            clusterRingSetNodeWeight(ring, 9, 900);

            size_t len = 0;
            uint8_t *snap = persistOpsClusterRing.snapshot(ring, &len);
            clusterRing *back =
                (clusterRing *)persistOpsClusterRing.restore(snap, len);
            if (!back) {
                ERR("strategy %d: restore refused its own snapshot", (int)s);
                zfree(snap);
                continue;
            }

            size_t len2 = 0;
            uint8_t *snap2 = persistOpsClusterRing.snapshot(back, &len2);
            if (len2 != len || memcmp(snap, snap2, len) != 0) {
                ERR("strategy %d: snapshot round trip changed bytes", (int)s);
            }
            zfree(snap2);
            zfree(snap);

            for (uint32_t k = 0; k < 500; k++) {
                char key[16];
                const int keyLen = snprintf(key, sizeof(key), "key%u", k);
                clusterPlacement a;
                clusterPlacement b;
                const clusterResult ra =
                    clusterRingLocate(ring, key, (uint32_t)keyLen, &a);
                const clusterResult rb =
                    clusterRingLocate(back, key, (uint32_t)keyLen, &b);
                if (ra != CLUSTER_OK || rb != CLUSTER_OK || !a.primary ||
                    !b.primary || a.primary->id != b.primary->id) {
                    ERR("strategy %d: key %u placed differently", (int)s, k);
                    k = 500;
                }
                clusterPlacementFree(&a);
                clusterPlacementFree(&b);
            }
            clusterRingFree(back);
        }
    }

    /* Benchmark tests */
    err += testBenchmarkLocate();

    /* Optimization edge case tests */
    err += testOptimizationEdgeCases();

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
