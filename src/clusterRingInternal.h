#pragma once

/* ====================================================================
 * Cluster Ring Internal Structures
 * ====================================================================
 * Implementation details - not part of public API
 */

#include "clusterRing.h"
#include "multiarray.h"
#include "multidict.h"

#include <pthread.h>
#include <stdatomic.h>

__BEGIN_DECLS

/* ====================================================================
 * Internal Node Structure
 * ==================================================================== */

struct clusterNode {
    uint64_t id;
    mdsc *name;
    mdsc *address;

    clusterLocation location;

    /* Capacity */
    uint32_t weight;
    uint64_t capacityBytes;
    uint64_t usedBytes;

    /* State */
    clusterNodeState state;
    uint64_t stateChangedAt;
    uint32_t failureCount;

    /* Health tracking */
    clusterNodeHealth lastHealth;
    clusterNodeLoad lastLoad;
    uint64_t lastHealthCheck;
    uint64_t lastLoadCheck;

    /* Virtual nodes (for Ketama) */
    uint32_t vnodeCount;
    uint32_t vnodeStartIndex; /* Index into ring's vnode array */

    /* Current position in ring->nodeArray. Maintained on add and on
     * compaction so nodeIdToIndex() is O(1) (via the nodeById dict) instead
     * of an O(nodeCount) scan on every routing probe. Transient/runtime —
     * not serialized; rebuilt when nodes are re-added on deserialize. */
    uint32_t arrayIndex;
};

/* ====================================================================
 * Virtual Node (for ring-based algorithms)
 * ==================================================================== */

/* 24 bytes: the owning node id is read through nodePtr->id (vnodes are
 * always rebuilt from live nodes, and RemoveVnodes runs before its
 * node is freed), so the ring's vnode array stays 25% smaller and the
 * hashPoint binary search touches fewer cache lines. */
typedef struct clusterVnode {
    uint64_t hashPoint;   /* Position on hash ring */
    clusterNode *nodePtr; /* Direct pointer to node (avoids hash lookup) */
    uint16_t vnodeIndex;  /* Index within node's vnodes */
    uint16_t _pad[3];
} clusterVnode;

/* ====================================================================
 * Keyspace Internal
 * ==================================================================== */

struct clusterKeySpace {
    mdsc *name;
    uint32_t id;

    clusterQuorum quorum;
    clusterStrategyType strategy;

    clusterAffinityRule *rules;
    uint8_t ruleCount;

    /* Stats */
    uint64_t locateCount;
    uint64_t writeCount;
    uint64_t readCount;
};

/* ====================================================================
 * Rebalance Plan Internal
 * ==================================================================== */

struct clusterRebalancePlan {
    clusterRebalanceMove *moves;
    uint32_t moveCount;
    uint32_t moveCapacity;

    uint32_t completedCount;
    uint32_t failedCount;

    uint64_t totalBytes;
    uint64_t movedBytes;

    uint64_t createdAt;
    uint64_t startedAt;
};

/* ====================================================================
 * Strategy-Specific Data
 * ==================================================================== */

typedef struct clusterKetamaData {
    clusterVnode *vnodes; /* Sorted by hashPoint */
    uint32_t vnodeCount;
    uint32_t vnodeCapacity;
    bool needsSort; /* Dirty flag after modifications */
} clusterKetamaData;

/* Healthy nodes in ring order: Jump's buckets */
typedef struct clusterJumpData {
    const clusterNode **nodes;
    uint32_t bucketCount;
} clusterJumpData;

/* Maglev's lookup table of healthy nodes (NULL: no healthy node).  A node
 * change marks it stale; the first placement after rebuilds it under
 * `lock` (double-checked on `stale`), so a run of changes costs one
 * rebuild and concurrent placements stay safe. */
typedef struct clusterMaglevData {
    const clusterNode **lookup;
    uint32_t tableSize; /* Prime number size */
    _Atomic bool stale;
    pthread_mutex_t lock;
} clusterMaglevData;

/* ====================================================================
 * Main Ring Structure
 * ==================================================================== */

struct clusterRing {
    mdsc *name;

    /* Nodes */
    multidict *nodeById;     /* nodeId -> clusterNode* */
    clusterNode **nodeArray; /* Dense array for iteration */
    uint32_t nodeCount;
    uint32_t nodeCapacity;
    uint32_t healthyNodeCount;

    /* Strategy: the ring's own (never DEFAULT) and its custom hooks */
    clusterStrategyType strategyType;
    clusterStrategy *customStrategy;

    /* Placement state per strategy, live while some user (the ring itself
     * or a keyspace) places with a strategy that needs it, and current
     * after every node change: the vnode ring (Ketama and bounded-load),
     * Jump's buckets and Maglev's table.  Rendezvous and custom keep none.
     * The bounded-load fill sum (usedBytes / capacityBytes over healthy
     * nodes with a capacity) is always kept. */
    uint32_t strategyUsers[CLUSTER_STRATEGY_COUNT];
    clusterKetamaData vnodes;
    clusterJumpData jump;
    clusterMaglevData maglev;
    double fillSum;
    uint32_t fillNodes;

    /* Configuration */
    clusterVnodeConfig vnodeConfig;
    clusterQuorum defaultQuorum;
    uint32_t hashSeed;
    uint32_t boundedLoadPercent; /* bounded-load: pass over nodes whose fill
                                    is above this % of the mean fill */

    /* Affinity */
    clusterAffinityRule *affinityRules;
    uint8_t affinityRuleCount;

    /* Keyspaces */
    multidict *keySpaceByName;
    clusterKeySpace **keySpaces;
    uint32_t keySpaceCount;
    uint32_t keySpaceCapacity;

    /* Rebalancing */
    clusterRebalancePlan *rebalancePlan;
    bool rebalanceInProgress;

    /* Health provider */
    clusterHealthProvider *healthProvider;
    bool loadAwareRouting;

    /* Callbacks */
    clusterNodeStateCallback stateCallback;
    void *stateCallbackData;
    clusterRebalanceCallback rebalanceCallback;
    void *rebalanceCallbackData;

    /* Version for delta serialization */
    uint64_t version;
    uint64_t lastModified;

    /* Stats.  Placement reads may run concurrently, so the counters they
     * bump are relaxed atomics */
    clusterRingStats stats;
    _Atomic uint64_t locateOps;
    _Atomic uint64_t writeOps;
    _Atomic uint64_t readOps;
};

/* ====================================================================
 * Internal Functions
 * ==================================================================== */

/* Strategy-specific locate: up to maxNodes distinct healthy nodes in the
 * strategy's preference order (the strategy's state must be live) */
uint32_t clusterLocateKetama(const clusterRing *ring, const void *key,
                             uint32_t keyLen, const clusterNode **out,
                             uint32_t maxNodes);

uint32_t clusterLocateJump(const clusterRing *ring, const void *key,
                           uint32_t keyLen, const clusterNode **out,
                           uint32_t maxNodes);

uint32_t clusterLocateMaglev(const clusterRing *ring, const void *key,
                             uint32_t keyLen, const clusterNode **out,
                             uint32_t maxNodes);

uint32_t clusterLocateRendezvous(const clusterRing *ring, const void *key,
                                 uint32_t keyLen, const clusterNode **out,
                                 uint32_t maxNodes);

uint32_t clusterLocateBounded(const clusterRing *ring, const void *key,
                              uint32_t keyLen, const clusterNode **out,
                              uint32_t maxNodes);

/* Virtual node management */
clusterResult clusterRingAddVnodes(clusterRing *ring, clusterNode *node);
clusterResult clusterRingRemoveVnodes(clusterRing *ring, uint64_t nodeId);
void clusterRingSortVnodes(clusterRing *ring);

/* Hash functions */
uint64_t clusterHash64(const void *key, uint32_t len, uint32_t seed);
uint32_t clusterHash32(const void *key, uint32_t len, uint32_t seed);

/* Affinity checking */
bool clusterCheckAffinity(const clusterRing *ring, const clusterNode **nodes,
                          uint32_t count, const clusterAffinityRule *rules,
                          uint8_t ruleCount);

/* Rebalance planning */
clusterRebalancePlan *clusterPlanRebalanceKetama(const clusterRing *ring,
                                                 const clusterNode *added,
                                                 const clusterNode *removed);

/* Node array management */
clusterResult clusterRingGrowNodeArray(clusterRing *ring);
void clusterRingCompactNodeArray(clusterRing *ring);

/* Timing helpers */
uint64_t clusterGetTimeNs(void);

__END_DECLS
