/* clusterRingContractTest.c - persistOpsClusterRing in the persistence
 * contract
 *
 * Snapshot-only: every topology change is made durable with a new
 * snapshot, so contracts 1, 3 and 4 (node names) apply. */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../clusterRing.h"
#include "../../str.h"

#include <stdio.h>
#include <string.h>

/* A seeded affinity rule list of 0..3 rules */
static uint8_t pcRingRules(uint64_t *rng, clusterAffinityRule rules[3]) {
    const uint8_t count = (uint8_t)(splitmix64(rng) % 4);
    for (uint8_t r = 0; r < count; r++) {
        rules[r].spreadLevel =
            (clusterTopologyLevel)(splitmix64(rng) % CLUSTER_LEVEL_COUNT);
        rules[r].minSpread = (uint8_t)splitmix64(rng);
        rules[r].required = splitmix64(rng) & 1;
    }
    return count;
}

/* A seeded valid quorum: W, sync and R at most N */
static clusterQuorum pcRingQuorum(uint64_t *rng) {
    const uint8_t n = (uint8_t)(1 + splitmix64(rng) % 7);
    return (clusterQuorum){
        .replicaCount = n,
        .writeQuorum = (uint8_t)(splitmix64(rng) % (n + 1u)),
        .writeSync = (uint8_t)(splitmix64(rng) % (n + 1u)),
        .readQuorum = (uint8_t)(splitmix64(rng) % (n + 1u)),
        .readRepairEnabled = splitmix64(rng) & 1,
        .consistency = (clusterConsistencyLevel)(splitmix64(rng) % 6),
    };
}

static void *pcRingBuild(const persistContractBuild *b) {
    uint64_t rng = b->seed;
    clusterAffinityRule ringRules[3];
    const clusterRingConfig config = {
        .name = (b->seed & 2) ? "ring" : "",
        .strategyType =
            (clusterStrategyType)(b->seed % CLUSTER_STRATEGY_CUSTOM),
        .vnodes = {.vnodeMultiplier = (uint32_t)(splitmix64(&rng) % 300),
                   .minVnodesPerNode = 1 + (uint32_t)(splitmix64(&rng) % 8),
                   .maxVnodesPerNode = 8 + (uint32_t)(splitmix64(&rng) % 64),
                   .replicaPointSpread = splitmix64(&rng) & 1},
        .defaultQuorum = pcRingQuorum(&rng),
        .affinityRules = ringRules,
        .affinityRuleCount = pcRingRules(&rng, ringRules),
        .hashSeed = (uint32_t)splitmix64(&rng),
        .boundedLoadPercent = 100 + (uint32_t)(splitmix64(&rng) % 200),
    };
    /* @own: caller; free=clusterRingFree */
    clusterRing *ring = clusterRingNew(&config);
    persistContractStart(b->log, ring);

    /* Nodes join, leave, change state and weight; keyspaces come and go */
    const uint64_t idSpace = 2 + b->size;
    for (size_t i = 0; i < b->size; i++) {
        const uint64_t id = splitmix64(&rng) % idSpace;
        const uint64_t roll = splitmix64(&rng) % 16;
        char name[32];
        char address[32];
        if (roll < 8) {
            snprintf(name, sizeof(name), "node-%" PRIu64, id);
            snprintf(address, sizeof(address), "10.0.%" PRIu64 ".%" PRIu64,
                     id >> 8, id & 255);
            const clusterNodeConfig nc = {
                .id = id,
                .name = name,
                .address = (roll & 1) ? address : NULL,
                .location = {.nodeId = id,
                             .rackId = (uint32_t)splitmix64(&rng),
                             .dcId = (uint32_t)(splitmix64(&rng) % 4),
                             .azId = (uint32_t)(splitmix64(&rng) % 3),
                             .countryId = (uint16_t)splitmix64(&rng),
                             .continentId = (uint8_t)splitmix64(&rng)},
                .weight = (uint32_t)(splitmix64(&rng) % 400),
                .capacityBytes = splitmix64(&rng),
                .initialState =
                    (clusterNodeState)(splitmix64(&rng) %
                                       (CLUSTER_NODE_MAINTENANCE + 1)),
            };
            clusterRingAddNode(ring, &nc);
        } else if (roll < 10) {
            clusterRingRemoveNode(ring, id);
        } else if (roll < 12) {
            clusterRingSetNodeState(
                ring, id,
                (clusterNodeState)(splitmix64(&rng) %
                                   (CLUSTER_NODE_MAINTENANCE + 1)));
        } else if (roll == 12) {
            clusterRingSetNodeWeight(ring, id,
                                     (uint32_t)(splitmix64(&rng) % 400));
        } else if (roll == 13) {
            clusterRingSetNodeUsedBytes(ring, id, splitmix64(&rng) >> 4);
        } else if (roll == 14) {
            snprintf(name, sizeof(name), "ks-%" PRIu64, id % 32);
            clusterAffinityRule rules[3];
            const clusterKeySpaceConfig kc = {
                .name = name,
                .quorum = pcRingQuorum(&rng),
                .rules = rules,
                .ruleCount = pcRingRules(&rng, rules),
                .strategy = (clusterStrategyType)(splitmix64(&rng) %
                                                  CLUSTER_STRATEGY_CUSTOM),
            };
            clusterRingAddKeySpace(ring, &kc, NULL);
        } else {
            snprintf(name, sizeof(name), "ks-%" PRIu64, id % 32);
            clusterRingRemoveKeySpace(ring, name);
        }
    }

    /* Each string names a node and a keyspace */
    for (size_t i = 0; i < b->stringCount; i++) {
        char text[128];
        const size_t len = b->strings[i].len < sizeof(text) - 1
                               ? b->strings[i].len
                               : sizeof(text) - 1;
        memcpy(text, databoxBytes(&b->strings[i]), len);
        text[len] = '\0';
        const clusterNodeConfig nc = {.id = idSpace + i, .name = text};
        clusterRingAddNode(ring, &nc);
        const clusterKeySpaceConfig kc = {.name = text,
                                          .quorum = CLUSTER_QUORUM_BALANCED};
        clusterRingAddKeySpace(ring, &kc, NULL);
    }
    return ring;
}

typedef struct pcRingFind {
    const databox *bytes;
    bool found;
} pcRingFind;

static bool pcRingFindName(const clusterNode *node, void *userData) {
    pcRingFind *find = userData;
    const databox name = databoxNewBytesString(clusterNodeGetName(node));
    find->found = persistContractSameBytes(&name, find->bytes);
    return !find->found;
}

static bool pcRingHoldsBytes(const void *structure, const databox *bytes) {
    pcRingFind find = {.bytes = bytes};
    clusterRingIterateNodes(structure, pcRingFindName, &find);
    return find.found;
}

const persistContractType persistContractClusterRing = {
    .ops = &persistOpsClusterRing,
    .build = pcRingBuild,
    .holdsBytes = pcRingHoldsBytes,
};

#endif /* DATAKIT_TEST */
