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

## Placement strategies

| Strategy                      | Placement                                    | Rebalance planning   |
| ----------------------------- | -------------------------------------------- | -------------------- |
| `CLUSTER_STRATEGY_KETAMA`     | hash ring with virtual nodes (weight-scaled) | exact hash ranges    |
| `CLUSTER_STRATEGY_JUMP`       | jump consistent hash, no vnodes              | re-locate keys       |
| `CLUSTER_STRATEGY_RENDEZVOUS` | highest random weight                        | re-locate keys       |
| `CLUSTER_STRATEGY_MAGLEV`     | Maglev lookup table                          | re-locate keys       |
| `CLUSTER_STRATEGY_BOUNDED`    | consistent hashing with a load bound         | re-locate keys       |
| `CLUSTER_STRATEGY_CUSTOM`     | your `clusterStrategy` hooks                 | your `planRebalance` |

A keyspace (`clusterRingAddKeySpace`) can place with a strategy, quorum and affinity rules of its own;
`clusterRingLocateKeyspace` places under them.

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

`clusterRingAddNode(s)`, `RemoveNode`, `SetNodeState`, `SetNodeWeight`, `SetNodeUsedBytes`; reads through
`clusterRingGetNode` and the `clusterNodeGet*` accessors. Health comes from `clusterRingUpdateNodeHealth` /
`UpdateNodeLoad`, or a provider polled by `clusterRingPollHealth`. Placement skips unhealthy nodes. State
and rebalance callbacks (`clusterRingSetNodeStateCallback`, `SetRebalanceCallback`) report changes;
`clusterRingIterateNodes(ByState, ByLocation)` walks them; `clusterRingGetStats` summarizes.

## Rebalancing after topology changes

1. Keep a copy from when data last settled: `settled = clusterRingCopy(ring)`.
2. Change the topology (add, remove, reweight nodes) on `ring`.
3. `clusterRingPlanRebalance(ring, settled)` lists the moves: hash ranges, source and target node, an
   estimate of bytes (KETAMA plans exact ranges; other built-in strategies return
   `CLUSTER_ERR_UNSUPPORTED`: re-locate each key in both rings instead).
4. For each move: `clusterRingStartMove`, copy the range, then `clusterRingCompleteMove` (or `FailMove`, and
   start it again later). `clusterRebalancePlanProgress` / `GetInfo` report progress; `CancelRebalance`
   drops the plan.
5. When every move is complete, the data sits where `ring` places it: `ring` is the next settled copy.

A removed node keeps serving its data until the plan completes.

## Concurrency and storage

- Placements (`Locate*`, `PlanWrite`, `PlanRead`) may run concurrently with each other; any call that
  changes the ring needs exclusive access (a reader-writer lock fits).
- `clusterRingSerialize` / `clusterRingDeserialize` and the persist descriptor carry topology,
  configuration and keyspaces (not a running plan); `clusterRingGetVersion` changes with the topology.
