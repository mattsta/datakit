#pragma once

/* ====================================================================
 * Cluster Ring: Consistent Hashing State Machine
 * ====================================================================
 *
 * OVERVIEW
 * --------
 * A system-level abstraction for data-structure-driven consistent hashing
 * logic. Provides a pure API layer for distributed data placement without
 * implementing network I/O.
 *
 * Features:
 *   - Topology-aware placement (Node/Rack/Cage/DC/AZ/Region/Country/Continent)
 *   - Configurable replication with Write-N/Read-Y quorum semantics
 *   - Pluggable ring algorithms (Ketama, Jump, Rendezvous, Maglev, Bounded)
 *   - Data-type-aware routing via keyspaces
 *   - Node lifecycle management with minimal data movement
 *
 * DESIGN PRINCIPLES
 * -----------------
 *   - Opaque types: Implementation hidden from users
 *   - Zero network dependencies: Pure algorithmic API
 *   - Self-managing: health-aware routing, per-strategy state kept current
 *   - Scale-aware: Optimized for both small and large clusters
 *
 * THREAD SAFETY
 * -------------
 * Placements (Locate*, PlanWrite, PlanRead) may run concurrently with each
 * other; every call that changes the ring needs exclusive access.
 *
 * USAGE EXAMPLE
 * -------------
 *   clusterRingConfig config = {
 *       .name = "cache-ring",
 *       .strategyType = CLUSTER_STRATEGY_KETAMA,
 *       .defaultQuorum = CLUSTER_QUORUM_BALANCED,
 *   };
 *   clusterRing *ring = clusterRingNew(&config);
 *
 *   clusterNodeConfig node = {.id = 1, .name = "a", .weight = 100};
 *   clusterRingAddNode(ring, &node);
 *
 *   clusterPlacement p;
 *   clusterRingLocate(ring, "key", 3, &p);
 *   // clusterPlacementGetReplica(&p, 0..ReplicaCount-1): primary first
 *   clusterPlacementFree(&p);
 *
 *   clusterRingFree(ring);
 */

#include "databox.h"
#include "mdsc.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

__BEGIN_DECLS

/* ====================================================================
 * Forward Declarations (Opaque Types)
 * ==================================================================== */

typedef struct clusterRing clusterRing;
typedef struct clusterNode clusterNode;
typedef struct clusterKeySpace clusterKeySpace;
typedef struct clusterRebalancePlan clusterRebalancePlan;

/* ====================================================================
 * Result Codes
 * ==================================================================== */

typedef enum [[nodiscard]] clusterResult {
    CLUSTER_OK = 0,
    CLUSTER_ERR = -1,
    CLUSTER_ERR_NOT_FOUND = -2,
    CLUSTER_ERR_EXISTS = -3,
    CLUSTER_ERR_NO_NODES = -4,
    CLUSTER_ERR_QUORUM_FAILED = -5,
    CLUSTER_ERR_INVALID_STATE = -6,
    CLUSTER_ERR_ALLOC_FAILED = -7,
    CLUSTER_ERR_INVALID_CONFIG = -8,
    CLUSTER_ERR_AFFINITY = -9,     /* no replica set meets a required affinity
                                      rule (too few distinct locations) */
    CLUSTER_ERR_UNSUPPORTED = -10, /* the strategy cannot do this */
} clusterResult;

/* ====================================================================
 * Topology Hierarchy
 * ==================================================================== */

/* 8-level topology hierarchy for placement constraints */
typedef enum clusterTopologyLevel {
    CLUSTER_LEVEL_NODE = 0,          /* Physical/virtual machine */
    CLUSTER_LEVEL_RACK,              /* Network rack (failure domain) */
    CLUSTER_LEVEL_CAGE,              /* Physical cage/row */
    CLUSTER_LEVEL_DATACENTER,        /* Single datacenter */
    CLUSTER_LEVEL_AVAILABILITY_ZONE, /* AZ within region */
    CLUSTER_LEVEL_REGION,            /* Geographic region */
    CLUSTER_LEVEL_COUNTRY,           /* Country (legal/compliance) */
    CLUSTER_LEVEL_CONTINENT,         /* Continental failure domain */
    CLUSTER_LEVEL_COUNT
} clusterTopologyLevel;

/* Full topology path for a node */
typedef struct clusterLocation {
    uint64_t nodeId;
    uint32_t rackId;
    uint32_t cageId;
    uint32_t dcId;
    uint32_t azId;
    uint32_t regionId;
    uint16_t countryId;
    uint8_t continentId;
    uint8_t _pad;
} clusterLocation;

/* ====================================================================
 * Node States
 * ==================================================================== */

typedef enum clusterNodeState {
    CLUSTER_NODE_UP = 0,      /* Healthy, accepting traffic */
    CLUSTER_NODE_JOINING,     /* Joining cluster, receiving data */
    CLUSTER_NODE_LEAVING,     /* Graceful leave, draining traffic */
    CLUSTER_NODE_DOWN,        /* Unreachable, not accepting traffic */
    CLUSTER_NODE_SUSPECT,     /* Potentially down (gossip timeout) */
    CLUSTER_NODE_RECOVERING,  /* Recovering from failure */
    CLUSTER_NODE_MAINTENANCE, /* Planned maintenance window */
} clusterNodeState;

/* ====================================================================
 * Node Definition
 * ==================================================================== */

/* Node configuration for adding nodes to ring.
 * Strings are copied internally, caller retains ownership. */
typedef struct clusterNodeConfig {
    uint64_t id;         /* Unique node identifier */
    const char *name;    /* Human-readable name (copied) */
    const char *address; /* Network address (copied) */

    clusterLocation location; /* Topology placement */

    uint32_t weight;        /* Relative capacity (affects vnodes); 0 = 100 */
    uint64_t capacityBytes; /* Total storage capacity */

    clusterNodeState initialState; /* Starting state */
} clusterNodeConfig;

/* ====================================================================
 * Consistency & Quorum
 * ==================================================================== */

typedef enum clusterConsistencyLevel {
    CLUSTER_CONSISTENCY_ONE = 0,      /* Any single replica */
    CLUSTER_CONSISTENCY_QUORUM,       /* Majority (N/2 + 1) */
    CLUSTER_CONSISTENCY_ALL,          /* All replicas must respond */
    CLUSTER_CONSISTENCY_LOCAL_QUORUM, /* Majority within local DC */
    CLUSTER_CONSISTENCY_EACH_QUORUM,  /* Majority in each DC */
    CLUSTER_CONSISTENCY_LOCAL_ONE,    /* One replica in local DC */
} clusterConsistencyLevel;

typedef struct clusterQuorum {
    /* Write requirements */
    uint8_t replicaCount; /* Total replicas to maintain (N) */
    uint8_t writeQuorum;  /* Required acks for success (W) */
    uint8_t writeSync;    /* Synchronous writes required */

    /* Read requirements */
    uint8_t readQuorum;     /* Required responses (R) */
    bool readRepairEnabled; /* Auto-heal inconsistencies */

    /* Consistency level (alternative to explicit W/R) */
    clusterConsistencyLevel consistency;
} clusterQuorum;

/* Pre-defined quorum profiles */
extern const clusterQuorum CLUSTER_QUORUM_STRONG;   /* W=ALL, R=1 */
extern const clusterQuorum CLUSTER_QUORUM_EVENTUAL; /* W=1, R=1 */
extern const clusterQuorum CLUSTER_QUORUM_BALANCED; /* W=QUORUM, R=QUORUM */
extern const clusterQuorum
    CLUSTER_QUORUM_READ_HEAVY; /* W=ALL, R=1, read-repair */
extern const clusterQuorum CLUSTER_QUORUM_WRITE_HEAVY; /* W=1, R=ALL */

/* ====================================================================
 * Placement Strategies
 * ==================================================================== */

/* A ring places with one strategy; each keyspace may place with another.
 * The ring keeps each strategy's state (Ketama and bounded-load share one
 * vnode ring) while some user places with it, and updates every live
 * strategy on each node change, so a keyspace places exactly as a ring of
 * its strategy over the same nodes would. */
typedef enum clusterStrategyType {
    CLUSTER_STRATEGY_DEFAULT = 0, /* ring: Ketama; keyspace: the ring's */
    CLUSTER_STRATEGY_KETAMA,      /* Classic consistent hashing + vnodes */
    CLUSTER_STRATEGY_JUMP,        /* Jump consistent hash (no vnodes) */
    CLUSTER_STRATEGY_RENDEZVOUS,  /* Highest Random Weight */
    CLUSTER_STRATEGY_MAGLEV,      /* Google Maglev lookup tables */
    CLUSTER_STRATEGY_BOUNDED,     /* Bounded-load consistent hashing */
    CLUSTER_STRATEGY_CUSTOM,      /* The ring's clusterStrategy hooks */
    CLUSTER_STRATEGY_COUNT
} clusterStrategyType;

/* Most virtual nodes one node can own */
#define CLUSTER_VNODES_PER_NODE_MAX 65536u

/* Virtual node configuration (for Ketama and bounded-load).  A zero field
 * takes its default (multiplier 150, min 10, max 500); clusterRingNew
 * refuses min > max and max > CLUSTER_VNODES_PER_NODE_MAX. */
typedef struct clusterVnodeConfig {
    uint32_t vnodeMultiplier;  /* vnodes = weight * multiplier / 100 */
    uint32_t minVnodesPerNode; /* Floor for low-weight nodes */
    uint32_t maxVnodesPerNode; /* Cap for high-weight nodes */
    bool replicaPointSpread;   /* Spread replica points evenly */
} clusterVnodeConfig;

/* Custom strategy interface */
typedef struct clusterStrategy clusterStrategy;

typedef uint32_t (*clusterLocateFn)(const clusterRing *ring, const void *key,
                                    uint32_t keyLen, const clusterNode **out,
                                    uint32_t maxNodes);

typedef struct clusterRebalanceMove clusterRebalanceMove;

/* Fill `plan` (clusterRebalancePlanAddMove) with the moves that take data
 * placed by `from` to where `to` places it; false refuses the plan */
typedef bool (*clusterRebalanceFn)(const clusterRing *from,
                                   const clusterRing *to,
                                   clusterRebalancePlan *plan);

/* A CUSTOM strategy: `locate` ranks up to maxNodes healthy nodes for a key
 * (the first replicaCount are its replicas); `preferenceOrder`, when set,
 * ranks every healthy node and is used instead when affinity rules need
 * candidates past the replicas; `planRebalance`, when set, serves
 * clusterRingPlanRebalance. */
struct clusterStrategy {
    const char *name;
    clusterLocateFn locate;
    clusterLocateFn preferenceOrder;
    clusterRebalanceFn planRebalance;
    void *strategyData;
    void (*freeStrategyData)(void *data);
};

/* ====================================================================
 * Affinity Rules (Replica Spread Constraints)
 * ==================================================================== */

typedef struct clusterAffinityRule {
    clusterTopologyLevel spreadLevel; /* Minimum spread level */
    uint8_t minSpread;                /* Min distinct values at level */
    bool required;                    /* Hard vs soft constraint */
} clusterAffinityRule;

/* Common affinity presets */
extern const clusterAffinityRule CLUSTER_AFFINITY_RACK_SPREAD;
extern const clusterAffinityRule CLUSTER_AFFINITY_AZ_SPREAD;
extern const clusterAffinityRule CLUSTER_AFFINITY_REGION_SPREAD;

/* ====================================================================
 * Ring Configuration
 * ==================================================================== */

/* Ring configuration for creating a new cluster ring.
 * Strings are copied internally, caller retains ownership. */
typedef struct clusterRingConfig {
    const char *name; /* Ring identifier (copied) */

    /* Strategy selection */
    clusterStrategyType strategyType; /* DEFAULT = Ketama */
    clusterStrategy *customStrategy;  /* For CLUSTER_STRATEGY_CUSTOM */

    /* Virtual nodes (for Ketama) */
    clusterVnodeConfig vnodes;

    /* Default consistency */
    clusterQuorum defaultQuorum;

    /* Affinity rules */
    clusterAffinityRule *affinityRules;
    uint8_t affinityRuleCount;

    /* Sizing hints */
    uint32_t expectedNodeCount; /* Pre-allocate for efficiency */
    uint32_t hashSeed;          /* Hash randomization seed */

    /* Bounded-load placement passes over a healthy node whose fill
     * (usedBytes / capacityBytes, see clusterRingSetNodeUsedBytes) is above
     * this percentage of the mean fill of healthy nodes with a capacity,
     * taking it only when no other node remains.  0 = 125; at least 100. */
    uint32_t boundedLoadPercent;
} clusterRingConfig;

/* ====================================================================
 * Placement Results
 * ==================================================================== */

/* Nodes a placement or plan names, in preference order (read them with the
 * accessors below; the owning result's Free releases them).  Results hold
 * no shared state, so placements and plans may run concurrently.  Every
 * placement and plan call initializes its result first, so the result is
 * readable (empty on a refusal) and safe to Free whatever it returns. */
typedef struct clusterNodeList {
    const clusterNode **nodes;
    uint32_t count;
    uint32_t capacity;
} clusterNodeList;

/* Placement result.  Use clusterPlacementFree() to release resources. */
typedef struct clusterPlacement {
    const clusterNode *primary; /* Primary replica */
    clusterNodeList replicas;   /* All replicas, primary first */
    uint32_t healthyCount;      /* Currently healthy */
    bool spreadMet;     /* every affinity rule (soft ones too) reached its
                           minSpread; required ones always do */
    uint64_t hashValue; /* Computed hash of key */
    const clusterKeySpace *keySpace; /* Resolved keyspace */
} clusterPlacement;

/* Write set: nodes that need to receive a write */
typedef struct clusterWriteSet {
    clusterNodeList targets;     /* Write targets */
    uint8_t syncRequired;        /* Sync acks required */
    uint8_t asyncAllowed;        /* Async writes allowed */
    uint32_t suggestedTimeoutMs; /* Suggested timeout */
} clusterWriteSet;

/* Read set: nodes to query for a read, in preference order */
typedef struct clusterReadSet {
    clusterNodeList candidates; /* Read candidates */
    uint8_t requiredResponses;  /* Responses needed */
    bool readRepair;            /* Do read repair if inconsistent */
} clusterReadSet;

/* Placement helper functions */
void clusterPlacementInit(clusterPlacement *p);
void clusterPlacementFree(clusterPlacement *p);
size_t clusterPlacementReplicaCount(const clusterPlacement *p);
const clusterNode *clusterPlacementGetReplica(const clusterPlacement *p,
                                              size_t idx);

/* Write/Read set helper functions */
void clusterWriteSetInit(clusterWriteSet *ws);
void clusterWriteSetFree(clusterWriteSet *ws);
size_t clusterWriteSetTargetCount(const clusterWriteSet *ws);
const clusterNode *clusterWriteSetGetTarget(const clusterWriteSet *ws,
                                            size_t idx);

void clusterReadSetInit(clusterReadSet *rs);
void clusterReadSetFree(clusterReadSet *rs);
size_t clusterReadSetCandidateCount(const clusterReadSet *rs);
const clusterNode *clusterReadSetGetCandidate(const clusterReadSet *rs,
                                              size_t idx);

/* ====================================================================
 * Ring Lifecycle
 * ==================================================================== */

/* Create ring with configuration.  Returns NULL for an unknown strategy,
 * a vnode configuration clusterVnodeConfig refuses, an affinity rule with
 * an unknown level, or a quorum that is not valid (see
 * clusterRingAddKeySpace); a zero defaultQuorum.replicaCount takes
 * CLUSTER_QUORUM_BALANCED. */
clusterRing *clusterRingNew(const clusterRingConfig *config);

/* Create ring with defaults */
clusterRing *clusterRingNewDefault(void);

/* Free ring and all resources */
void clusterRingFree(clusterRing *ring);

/* ====================================================================
 * Node Management
 * ==================================================================== */

/* Add a new node to the ring */
clusterResult clusterRingAddNode(clusterRing *ring,
                                 const clusterNodeConfig *config);

/* Remove a node from the ring */
clusterResult clusterRingRemoveNode(clusterRing *ring, uint64_t nodeId);

/* Batch add nodes (for cluster bootstrap) */
clusterResult clusterRingAddNodes(clusterRing *ring,
                                  const clusterNodeConfig *configs,
                                  uint32_t count);

/* Change node state */
clusterResult clusterRingSetNodeState(clusterRing *ring, uint64_t nodeId,
                                      clusterNodeState state);

/* Update node weight (its vnode share follows); 0 sets the default, 100 */
clusterResult clusterRingSetNodeWeight(clusterRing *ring, uint64_t nodeId,
                                       uint32_t weight);

/* Record how many of the node's capacityBytes are in use (persisted with
 * the ring; bounded-load placement reads the fill it gives, see
 * clusterRingConfig.boundedLoadPercent) */
clusterResult clusterRingSetNodeUsedBytes(clusterRing *ring, uint64_t nodeId,
                                          uint64_t usedBytes);

/* Get node by ID (NULL if not found) */
const clusterNode *clusterRingGetNode(const clusterRing *ring, uint64_t nodeId);

/* Node count accessors */
uint32_t clusterRingNodeCount(const clusterRing *ring);
uint32_t clusterRingHealthyNodeCount(const clusterRing *ring);

/* ====================================================================
 * Node Accessors
 * ==================================================================== */

uint64_t clusterNodeGetId(const clusterNode *node);
const char *clusterNodeGetName(const clusterNode *node);
const char *clusterNodeGetAddress(const clusterNode *node);
clusterNodeState clusterNodeGetState(const clusterNode *node);
uint32_t clusterNodeGetWeight(const clusterNode *node);
uint64_t clusterNodeGetCapacity(const clusterNode *node);
uint64_t clusterNodeGetUsedBytes(const clusterNode *node);
const clusterLocation *clusterNodeGetLocation(const clusterNode *node);

/* ====================================================================
 * Data Placement (Core API)
 * ==================================================================== */

/* Locate replicas for a key: defaultQuorum.replicaCount nodes from the
 * ring's strategy, chosen to meet the ring's affinity rules.  The
 * strategy's preference order decides among nodes; nodes that add a
 * location value a rule still needs (at the rule's level, until its
 * minSpread) are taken first, then the most preferred of the rest, and
 * the replicas keep preference order (primary first).  Returns
 * CLUSTER_ERR_AFFINITY when a required rule cannot be met (too few
 * distinct locations, or minSpread above the replica count); a soft rule
 * that cannot be met leaves out->spreadMet false. */
clusterResult clusterRingLocate(const clusterRing *ring, const void *key,
                                uint32_t keyLen, clusterPlacement *out);

/* Locate with databox key.  The key places by its value: a string's bytes
 * (embedded or not), a number's value bytes, true/false/null by type, so
 * equal keys always place alike.  CLUSTER_ERR for an offset box or a
 * 128-bit box without its value. */
clusterResult clusterRingLocateBox(const clusterRing *ring, const databox *key,
                                   clusterPlacement *out);

/* clusterRingLocate under a keyspace's settings: its strategy (the ring's
 * for DEFAULT), its quorum.replicaCount (the ring's default when 0) and
 * its affinity rules (the ring's when it has none) */
clusterResult clusterRingLocateKeyspace(const clusterRing *ring,
                                        const clusterKeySpace *ks,
                                        const void *key, uint32_t keyLen,
                                        clusterPlacement *out);

/* Bulk placement: placements[i] is clusterRingLocateBox of keys[i] (each
 * placement initialized by the call; free each successful one with
 * clusterPlacementFree).  Every key is attempted; returns CLUSTER_OK or
 * the last key's failure. */
clusterResult clusterRingLocateBulk(const clusterRing *ring,
                                    const databox *keys, uint32_t keyCount,
                                    clusterPlacement *placements);

/* ====================================================================
 * Routing Decisions
 * ==================================================================== */

/* Plan a write operation.  `quorum` (the ring default when NULL) gives
 * the replica count (the ring default when its replicaCount is 0) and
 * the sync/ack requirements; replicas meet the ring's affinity rules as
 * in clusterRingLocate. */
clusterResult clusterRingPlanWrite(const clusterRing *ring, const void *key,
                                   uint32_t keyLen, const clusterQuorum *quorum,
                                   clusterWriteSet *out);

/* Plan a read operation (replica count and affinity as in
 * clusterRingPlanWrite) */
clusterResult clusterRingPlanRead(const clusterRing *ring, const void *key,
                                  uint32_t keyLen, const clusterQuorum *quorum,
                                  clusterReadSet *out);

/* Select the read node from a placement: the least loaded healthy replica
 * among those with a load report (clusterRingUpdateNodeLoad or the
 * provider), else the first healthy replica */
clusterResult clusterRingSelectReadNode(const clusterRing *ring,
                                        const clusterPlacement *placement,
                                        const clusterNode **selected);

/* ====================================================================
 * Keyspace Management
 * ==================================================================== */

typedef struct clusterKeySpaceConfig {
    const char *name;           /* Keyspace identifier (copied) */
    clusterQuorum quorum;       /* Override default quorum */
    clusterAffinityRule *rules; /* Affinity constraints */
    uint8_t ruleCount;
    clusterStrategyType strategy; /* DEFAULT = the ring's strategy; CUSTOM
                                     = the ring's clusterStrategy hooks */
} clusterKeySpaceConfig;

/* Refuses (CLUSTER_ERR_INVALID_CONFIG) an unknown strategy, an affinity
 * rule with an unknown level, and a quorum that is not valid: a known
 * consistency level and, when replicaCount is not 0, writeQuorum,
 * writeSync and readQuorum each at most replicaCount. */
clusterResult clusterRingAddKeySpace(clusterRing *ring,
                                     const clusterKeySpaceConfig *config,
                                     clusterKeySpace **out);

clusterResult clusterRingRemoveKeySpace(clusterRing *ring, const char *name);

const clusterKeySpace *clusterRingGetKeySpace(const clusterRing *ring,
                                              const char *name);

/* ====================================================================
 * Rebalancing
 * ====================================================================
 * A plan lists the copies that take data from where one ring placed it
 * to where another places it.  Keep a copy of the ring (clusterRingCopy)
 * from when the data last settled; after any number of topology changes,
 * clusterRingPlanRebalance(ring, settled) plans from that copy to the
 * ring.  Run each move (Start, copy the range, Complete or Fail; a failed
 * move may start again); once every move completes, the data sits where
 * the ring places it, and the ring is the next settled copy.
 *
 * A move's range is inclusive over clusterPlacement.hashValue (the key's
 * clusterHash64 under the ring's hashSeed).  Plans cover the ring's own
 * placement (its strategy, defaultQuorum.replicaCount and affinity
 * rules); a keyspace placing differently needs its own accounting.  A
 * plan is runtime state: snapshots and copies do not carry it. */

typedef enum clusterMoveState {
    CLUSTER_MOVE_PENDING = 0,
    CLUSTER_MOVE_IN_PROGRESS,
    CLUSTER_MOVE_COMPLETED,
    CLUSTER_MOVE_FAILED,
} clusterMoveState;

struct clusterRebalanceMove {
    uint64_t rangeStart;     /* first hash of the range */
    uint64_t rangeEnd;       /* last hash of the range (inclusive) */
    uint64_t sourceNodeId;   /* a node holding the range's data */
    uint64_t targetNodeId;   /* the node that must come to hold it */
    uint64_t estimatedBytes; /* source usedBytes x the range's share of
                                what the source holds (0 if unknown) */
    clusterMoveState state;
};

/* A plan's progress */
typedef struct clusterRebalanceInfo {
    uint32_t moveCount;
    uint32_t completedCount;
    uint32_t failedCount; /* moves now FAILED */
    uint64_t totalBytes;  /* sum of estimatedBytes */
    uint64_t movedBytes;  /* estimatedBytes of the completed moves */
    uint64_t createdAtMs; /* monotonic ms */
    uint64_t startedAtMs; /* first StartMove; 0 before */
} clusterRebalanceInfo;

/* A copy of the ring's topology, configuration and keyspaces (as a
 * snapshot round trip would give); NULL on allocation failure */
clusterRing *clusterRingCopy(const clusterRing *ring);

/* Plan the moves from `from`'s placement to `ring`'s, replacing any plan.
 * For every range and every node `ring` places it on that `from` did not,
 * one move copies it from a node `from` placed it on: the first such node
 * that is UP in `ring` or no longer in it (a removed node still serves its
 * data until the plan completes), else `from`'s primary.  Adjacent ranges
 * with the same source and target are one move.
 *
 * KETAMA plans exactly (both rings KETAMA with the same hashSeed, else
 * CLUSTER_ERR_INVALID_CONFIG); CUSTOM asks the strategy's planRebalance;
 * Jump, Rendezvous, Maglev and bounded-load do not place by hash ranges
 * (CLUSTER_ERR_UNSUPPORTED: re-locate each key in both rings instead).
 * The rebalance callback sees the new plan. */
clusterResult clusterRingPlanRebalance(clusterRing *ring,
                                       const clusterRing *from);

/* Add a PENDING move to a plan (for a CUSTOM planRebalance) */
clusterResult clusterRebalancePlanAddMove(clusterRebalancePlan *plan,
                                          const clusterRebalanceMove *move);

/* The ring's plan; NULL when none */
const clusterRebalancePlan *
clusterRingGetRebalancePlan(const clusterRing *ring);

uint32_t clusterRebalancePlanMoveCount(const clusterRebalancePlan *plan);

const clusterRebalanceMove *
clusterRebalancePlanGetMove(const clusterRebalancePlan *plan, uint32_t index);

/* Completed share of the moves (1.0 for an empty plan) */
float clusterRebalancePlanProgress(const clusterRebalancePlan *plan);

void clusterRebalancePlanGetInfo(const clusterRebalancePlan *plan,
                                 clusterRebalanceInfo *info);

/* Move transitions: Start takes a PENDING or FAILED move IN_PROGRESS;
 * Complete and Fail end an IN_PROGRESS one (CLUSTER_ERR_INVALID_STATE
 * otherwise, CLUSTER_ERR_NOT_FOUND for an index past the plan).  The
 * rebalance callback runs after each Complete and Fail. */
clusterResult clusterRingStartMove(clusterRing *ring, uint32_t moveIndex);
clusterResult clusterRingCompleteMove(clusterRing *ring, uint32_t moveIndex);
clusterResult clusterRingFailMove(clusterRing *ring, uint32_t moveIndex);

/* Drop the plan (CLUSTER_ERR when there is none) */
clusterResult clusterRingCancelRebalance(clusterRing *ring);

/* ====================================================================
 * Health & Load Integration
 * ==================================================================== */

typedef struct clusterNodeHealth {
    bool reachable;
    uint32_t latencyMs;
    float errorRate; /* 0.0 - 1.0 */
    uint64_t lastCheckTime;
} clusterNodeHealth;

typedef struct clusterNodeLoad {
    float cpuUsage; /* 0.0 - 1.0 */
    float memoryUsage;
    float diskUsage;
    uint32_t activeConnections;
    uint64_t requestQueueDepth;
} clusterNodeLoad;

/* Health provider callback interface */
typedef clusterNodeHealth (*clusterHealthCheckFn)(void *providerData,
                                                  uint64_t nodeId);
typedef clusterNodeLoad (*clusterLoadCheckFn)(void *providerData,
                                              uint64_t nodeId);

typedef struct clusterHealthProvider {
    clusterHealthCheckFn checkHealth;
    clusterLoadCheckFn getLoad;
    void *providerData;
    void (*freeProvider)(void *data);
} clusterHealthProvider;

/* Install the provider clusterRingPollHealth asks (copied; NULL removes
 * it).  The ring owns providerData from here: it calls freeProvider on it
 * when the provider is replaced or removed and when the ring is freed. */
void clusterRingSetHealthProvider(clusterRing *ring,
                                  const clusterHealthProvider *provider);

/* Ask the provider about every node (checkHealth and getLoad, each when
 * set) and apply the answers as clusterRingUpdateNodeHealth/Load would.
 * CLUSTER_ERR when no provider is installed. */
clusterResult clusterRingPollHealth(clusterRing *ring);

/* Record a node's health.  Three consecutive unreachable reports mark an
 * UP node SUSPECT (it leaves placement); a reachable report clears the
 * count and returns a SUSPECT node to UP.  Other states change only by
 * clusterRingSetNodeState. */
clusterResult clusterRingUpdateNodeHealth(clusterRing *ring, uint64_t nodeId,
                                          const clusterNodeHealth *health);

/* Record a node's load; clusterRingSelectReadNode prefers the least
 * loaded (by cpuUsage) of the replicas with a load report */
clusterResult clusterRingUpdateNodeLoad(clusterRing *ring, uint64_t nodeId,
                                        const clusterNodeLoad *load);

/* A node's last recorded health and load (zeroed before the first) */
const clusterNodeHealth *clusterNodeGetHealth(const clusterNode *node);
const clusterNodeLoad *clusterNodeGetLoad(const clusterNode *node);

/* ====================================================================
 * Statistics & Observability
 * ==================================================================== */

typedef struct clusterRingStats {
    /* Ring state */
    uint32_t nodeCount;
    uint32_t healthyNodes;
    uint32_t vnodeCount;
    uint32_t keySpaceCount;

    /* Load distribution */
    float loadVariance; /* How evenly distributed */
    float maxNodeLoad;  /* Busiest node's share */
    float minNodeLoad;  /* Least busy node's share */

    /* Operation counters (lifetime) */
    uint64_t locateOps; /* placements, including each plan's own */
    uint64_t writeOps;
    uint64_t readOps;
    uint64_t rebalanceMoves;

    /* Placement time (nanoseconds) over a sample of one placement in 64:
     * mean, 99th percentile (within 25% above the true value) and the
     * slowest; all 0 until the first sample */
    uint64_t avgLocateNs;
    uint64_t p99LocateNs;
    uint64_t maxLocateNs;

    /* Memory */
    uint64_t memoryBytes;
} clusterRingStats;

/* Fill every field of 'stats' from 'ring'. A NULL 'ring' fills all zeros. */
void clusterRingGetStats(const clusterRing *ring, clusterRingStats *stats);

/* ====================================================================
 * State Change Callbacks
 * ==================================================================== */

typedef void (*clusterNodeStateCallback)(clusterRing *ring, uint64_t nodeId,
                                         clusterNodeState oldState,
                                         clusterNodeState newState,
                                         void *userData);

typedef void (*clusterRebalanceCallback)(clusterRing *ring,
                                         const clusterRebalancePlan *plan,
                                         void *userData);

void clusterRingSetNodeStateCallback(clusterRing *ring,
                                     clusterNodeStateCallback cb,
                                     void *userData);

void clusterRingSetRebalanceCallback(clusterRing *ring,
                                     clusterRebalanceCallback cb,
                                     void *userData);

/* ====================================================================
 * Node Iteration
 * ==================================================================== */

typedef bool (*clusterNodeIterFn)(const clusterNode *node, void *userData);

/* Iterate all nodes */
void clusterRingIterateNodes(const clusterRing *ring, clusterNodeIterFn fn,
                             void *userData);

/* Iterate nodes matching state */
void clusterRingIterateNodesByState(const clusterRing *ring,
                                    clusterNodeState state,
                                    clusterNodeIterFn fn, void *userData);

/* Iterate nodes at topology level */
void clusterRingIterateNodesByLocation(const clusterRing *ring,
                                       clusterTopologyLevel level,
                                       uint64_t levelValue,
                                       clusterNodeIterFn fn, void *userData);

/* ====================================================================
 * Serialization (for persist module integration)
 * ==================================================================== */

/* Serialize ring state to buffer */
size_t clusterRingSerialize(const clusterRing *ring, void *buf, size_t bufLen);

/* Get required buffer size for serialization */
size_t clusterRingSerializeSize(const clusterRing *ring);

/* Deserialize ring from buffer */
clusterRing *clusterRingDeserialize(const void *buf, size_t bufLen);

/* Get current ring version (monotonically increasing on each modification). */
uint64_t clusterRingGetVersion(const clusterRing *ring);

/* persistOps descriptor — routes clusterRingSerialize/Deserialize
 * through the standard persist + flexEnvelope pipeline so the
 * topology is integrity-protected end-to-end.  Use with
 * persistAttachKeyring (or persistSetIntegrityKey) for tamper
 * evidence across crash/restart boundaries.  See persist.h. */
extern const struct persistOps persistOpsClusterRing;

/* ====================================================================
 * Debugging / Testing
 * ==================================================================== */

#ifdef DATAKIT_TEST
void clusterRingRepr(const clusterRing *ring);
void clusterNodeRepr(const clusterNode *node);
void clusterPlacementRepr(const clusterPlacement *p);
int clusterRingTest(int argc, char *argv[]);
#endif

__END_DECLS
