# clusterRing: where data lives in a cluster

**Header**: `clusterRing.h` **Source**: `clusterRing.c` **Persistence**: `persistOpsClusterRing`

clusterRing decides which nodes hold a key: replica placement by consistent hashing, aware of topology
(node, rack, cage, datacenter, availability zone, region, country, continent), quorums and node health. It
does no networking: it answers "which nodes", and the caller moves the bytes.

## Quick start

```c
#include "clusterRing.h"

const clusterRingConfig config = {
    .name = "cache-ring",
    .strategyType = CLUSTER_STRATEGY_KETAMA,
    .defaultQuorum = CLUSTER_QUORUM_BALANCED, /* N=3, W=2, R=2 */
};
clusterRing *ring = clusterRingNew(&config);

const clusterNodeConfig a = {.id = 1, .name = "a", .weight = 100};
if (clusterRingAddNode(ring, &a) != CLUSTER_OK) { /* ... */ }

clusterPlacement p;
if (clusterRingLocate(ring, "user:42", 7, &p) == CLUSTER_OK) {
    for (size_t i = 0; i < clusterPlacementReplicaCount(&p); i++) {
        const clusterNode *n = clusterPlacementGetReplica(&p, i); /* primary first */
        /* send to clusterNodeGetAddress(n) */
    }
    clusterPlacementFree(&p);
}

clusterRingFree(ring);
```

Every result is a `clusterResult` (`CLUSTER_OK` or a named error) and must be checked.
`clusterRingNewDefault()` is a KETAMA ring with the BALANCED quorum.

- `clusterRingLocateBox(ring, &key, &p)` places a databox key by its value (a string's bytes, a
  number's value), so equal keys place alike whatever their storage.
- `clusterRingLocateBulk(ring, keys, count, placements)` places an array of databox keys; free each
  placement that succeeded.
- `clusterPlacementInit` zeroes a placement before a call that may not fill it; `clusterPlacementFree`
  releases its replica list.

## Writes and reads

```c
clusterWriteSet ws;
clusterWriteSetInit(&ws);
if (clusterRingPlanWrite(ring, "user:42", 7, nullptr, &ws) == CLUSTER_OK) {
    for (size_t i = 0; i < clusterWriteSetTargetCount(&ws); i++) {
        send(clusterWriteSetGetTarget(&ws, i)); /* done when ws.syncRequired ack */
    }
}
clusterWriteSetFree(&ws);

clusterReadSet rs;
clusterReadSetInit(&rs);
if (clusterRingPlanRead(ring, "user:42", 7, nullptr, &rs) == CLUSTER_OK) {
    /* ask candidates in order until rs.requiredResponses answer;
     * rs.readRepair: write the newest value back to the stale ones */
    const clusterNode *first = clusterReadSetGetCandidate(&rs, 0);
    const size_t candidates = clusterReadSetCandidateCount(&rs);
}
clusterReadSetFree(&rs);
```

A NULL quorum takes the ring's default; pass a `clusterQuorum` to override it for one call.

## Placement strategies

| Strategy                      | Placement                                    | Rebalance planning   |
| ----------------------------- | -------------------------------------------- | -------------------- |
| `CLUSTER_STRATEGY_KETAMA`     | hash ring with virtual nodes (weight-scaled) | exact hash ranges    |
| `CLUSTER_STRATEGY_JUMP`       | jump consistent hash, no vnodes              | re-locate keys       |
| `CLUSTER_STRATEGY_RENDEZVOUS` | highest random weight                        | re-locate keys       |
| `CLUSTER_STRATEGY_MAGLEV`     | Maglev lookup table                          | re-locate keys       |
| `CLUSTER_STRATEGY_BOUNDED`    | consistent hashing with a load bound         | re-locate keys       |
| `CLUSTER_STRATEGY_CUSTOM`     | your `clusterStrategy` hooks                 | your `planRebalance` |

A keyspace can place with a strategy, quorum and affinity rules of its own:

```c
clusterAffinityRule spread[] = {CLUSTER_AFFINITY_AZ_SPREAD};
const clusterKeySpaceConfig sessions = {
    .name = "sessions",
    .quorum = CLUSTER_QUORUM_EVENTUAL,
    .rules = spread,
    .ruleCount = 1,
};
clusterKeySpace *ks = nullptr;
if (clusterRingAddKeySpace(ring, &sessions, &ks) == CLUSTER_OK) {
    clusterRingLocateKeyspace(ring, ks, "s:9", 3, &p);
}
```

`clusterRingGetKeySpace(ring, "sessions")` finds one by name; `clusterRingRemoveKeySpace(ring, "sessions")`
drops it.

## Quorums and affinity

- Quorum profiles: `CLUSTER_QUORUM_STRONG` (W=all, R=1), `EVENTUAL` (W=1, R=1), `BALANCED` (W=R=majority,
  read repair), `READ_HEAVY`, `WRITE_HEAVY`, or a `clusterQuorum` of your own (W and R at most N).
- `clusterRingPlanWrite` / `clusterRingPlanRead` give the nodes and acknowledgements a write or read needs;
  `clusterRingSelectReadNode` picks the least-loaded healthy replica that reported load.
- Affinity rules spread replicas across a topology level: `CLUSTER_AFFINITY_RACK_SPREAD` and
  `CLUSTER_AFFINITY_AZ_SPREAD` (required: placement fails with `CLUSTER_ERR_AFFINITY` when too few distinct
  locations exist), `CLUSTER_AFFINITY_REGION_SPREAD` (soft: `spreadMet` reports it). Give each node its
  `clusterLocation`.

## Nodes and health

| Change                             | Call                                                                                 |
| ---------------------------------- | ------------------------------------------------------------------------------------ |
| add one node, or a bootstrap batch | `clusterRingAddNode(ring, &config)`, `clusterRingAddNodes(ring, configs, n)`         |
| remove a node                      | `clusterRingRemoveNode(ring, id)`                                                    |
| take a node out of placement       | `clusterRingSetNodeState(ring, id, CLUSTER_NODE_MAINTENANCE)` (or `LEAVING`, `DOWN`) |
| change its share of the ring       | `clusterRingSetNodeWeight(ring, id, weight)` (0 = the default, 100)                  |
| report its disk use                | `clusterRingSetNodeUsedBytes(ring, id, bytes)` (bounded-load reads it)               |

`clusterRingGetNode(ring, id)` returns a node (NULL when absent); `clusterRingNodeCount` and
`clusterRingHealthyNodeCount` count them. A node reads through `clusterNodeGetId`, `clusterNodeGetName`,
`clusterNodeGetAddress`, `clusterNodeGetState`, `clusterNodeGetWeight`, `clusterNodeGetCapacity`,
`clusterNodeGetUsedBytes`, `clusterNodeGetLocation` (its `clusterLocation`: rack, cage, datacenter, AZ,
region, country and continent ids), `clusterNodeGetHealth` and `clusterNodeGetLoad`.

Health:

- `clusterRingUpdateNodeHealth(ring, id, &health)` records a probe: three unreachable reports in a row
  mark an UP node SUSPECT (it leaves placement), and a reachable one brings it back.
- `clusterRingUpdateNodeLoad(ring, id, &load)` records load; `clusterRingSelectReadNode` prefers the
  replica with the lowest `cpuUsage`.
- Or install a `clusterHealthProvider` with `clusterRingSetHealthProvider` (the ring then owns its
  `providerData`) and call `clusterRingPollHealth` on a timer to probe every node through it.

`clusterRingSetNodeStateCallback` reports every state change and `clusterRingSetRebalanceCallback` every
plan change. `clusterRingIterateNodes` walks all nodes (the callback returns false to stop),
`clusterRingIterateNodesByState(ring, CLUSTER_NODE_DOWN, fn, ud)` those in one state, and
`clusterRingIterateNodesByLocation(ring, CLUSTER_LEVEL_RACK, 7, fn, ud)` those in one rack (or any
level). `clusterRingGetStats` summarizes counts, load spread, operation counters and placement latency.

## Rebalancing after topology changes

1. Keep a copy from when data last settled: `settled = clusterRingCopy(ring)`.
2. Change the topology (add, remove, reweight nodes) on `ring`.
3. `clusterRingPlanRebalance(ring, settled)` lists the moves: hash ranges, source and target node, an
   estimate of bytes (KETAMA plans exact ranges; other built-in strategies return
   `CLUSTER_ERR_UNSUPPORTED`: re-locate each key in both rings instead).
4. For each move: `clusterRingStartMove`, copy the range, then `clusterRingCompleteMove` (or
   `clusterRingFailMove`, and start it again later).
5. When every move is complete, the data sits where `ring` places it: `ring` is the next settled copy.

```c
if (clusterRingPlanRebalance(ring, settled) == CLUSTER_OK) {
    const clusterRebalancePlan *plan = clusterRingGetRebalancePlan(ring);
    for (uint32_t i = 0; i < clusterRebalancePlanMoveCount(plan); i++) {
        const clusterRebalanceMove *m = clusterRebalancePlanGetMove(plan, i);
        clusterRingStartMove(ring, i);
        if (copyRange(m)) {   /* the caller's transfer of the move's range */
            clusterRingCompleteMove(ring, i);
        } else {
            clusterRingFailMove(ring, i); /* Start it again later */
        }
    }

    clusterRebalanceInfo info;
    clusterRebalancePlanGetInfo(plan, &info); /* moves done and failed, bytes moved */
}
```

`clusterRebalancePlanProgress` is the completed share of the moves; `clusterRingCancelRebalance` drops the
plan. A `CLUSTER_STRATEGY_CUSTOM` strategy's `planRebalance` hook builds its plan with
`clusterRebalancePlanAddMove`. A removed node keeps serving its data until the plan completes.

## Concurrency and storage

- Placements (`Locate*`, `PlanWrite`, `PlanRead`) may run concurrently with each other; any call that
  changes the ring needs exclusive access (a reader-writer lock fits).
- `clusterRingSerialize` (into a buffer of `clusterRingSerializeSize` bytes) / `clusterRingDeserialize`
  and the persist descriptor carry topology,
  configuration and keyspaces (not a running plan); `clusterRingGetVersion` changes with the topology.
