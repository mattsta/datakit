#pragma once

/* ====================================================================
 * Cluster Ring Internal Structures
 * ====================================================================
 * Implementation details - not part of public API
 */

#include "clusterRing.h"
#include "multiarray.h"
#include "multidict.h"

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
};

/* ====================================================================
 * Virtual Node (for ring-based algorithms)
 * ==================================================================== */

typedef struct clusterVnode {
    uint64_t hashPoint;   /* Position on hash ring */
    uint64_t nodeId;      /* Owning node ID (for serialization) */
    clusterNode *nodePtr; /* Direct pointer to node (avoids hash lookup) */
    uint16_t vnodeIndex;  /* Index within node's vnodes */
    uint16_t _pad;
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

typedef struct clusterJumpData {
    uint64_t *nodeIds; /* Node IDs in bucket order */
    uint32_t bucketCount;
} clusterJumpData;

typedef struct clusterMaglevData {
    uint64_t *lookup;   /* Lookup table */
    uint32_t tableSize; /* Prime number size */
    bool needsRebuild;
} clusterMaglevData;

typedef struct clusterRendezvousData {
    /* No extra data needed - computed on-demand */
    uint32_t _unused;
} clusterRendezvousData;

typedef struct clusterBoundedData {
    float loadFactor;    /* Max load = avg * (1 + loadFactor) */
    uint64_t *nodeLoads; /* Current load per node */
    uint32_t nodeCount;
} clusterBoundedData;

typedef union clusterStrategyData {
    clusterKetamaData ketama;
    clusterJumpData jump;
    clusterMaglevData maglev;
    clusterRendezvousData rendezvous;
    clusterBoundedData bounded;
} clusterStrategyData;

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

    /* Strategy */
    clusterStrategyType strategyType;
    clusterStrategy *customStrategy;
    clusterStrategyData strategyData;

    /* Configuration */
    clusterVnodeConfig vnodeConfig;
    clusterQuorum defaultQuorum;
    uint32_t hashSeed;

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

    /* Stats */
    clusterRingStats stats;
};

/* ====================================================================
 * Internal Functions
 * ==================================================================== */

/* Strategy initialization */
clusterResult clusterRingInitKetama(clusterRing *ring);
clusterResult clusterRingInitJump(clusterRing *ring);
clusterResult clusterRingInitMaglev(clusterRing *ring);
clusterResult clusterRingInitRendezvous(clusterRing *ring);
clusterResult clusterRingInitBounded(clusterRing *ring);

/* Strategy cleanup */
void clusterRingFreeKetama(clusterRing *ring);
void clusterRingFreeJump(clusterRing *ring);
void clusterRingFreeMaglev(clusterRing *ring);
void clusterRingFreeRendezvous(clusterRing *ring);
void clusterRingFreeBounded(clusterRing *ring);

/* Strategy-specific locate */
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
