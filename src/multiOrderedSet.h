#pragma once

#include "databox.h"
#include "flexCapacityManagement.h"

/* ====================================================================
 * multiOrderedSet - Sorted set with O(1) member lookup
 * ====================================================================
 *
 * A sorted set stores (score, member) pairs where:
 *   - Members are unique (databox keys)
 *   - Scores are databox values (int64, uint64, double, etc.)
 *   - Pairs are sorted by score (ascending)
 *   - When scores are equal, pairs are sorted by member lexicographically
 *
 * ====================================================================
 * WHEN TO USE multiOrderedSet vs multimap
 * ====================================================================
 *
 * Use multiOrderedSet when you need:
 *   - Fast O(1) "does member X exist?" checks
 *   - Fast O(1) "what is member X's score?" lookups
 *   - Sorted iteration by score
 *   - Rank queries ("what rank is member X?")
 *
 * Use multimap directly when you only need:
 *   - Sorted storage with lookup by score (key)
 *   - Lower memory usage
 *   - Faster bulk insertions
 *
 * Example use cases for multiOrderedSet:
 *   - Leaderboards: lookup player score by player ID (member)
 *   - Rate limiters: check if IP exists, get its request count
 *   - Priority queues with named items: lookup priority by item name
 *   - Redis ZSET-style operations: ZSCORE, ZRANK, ZINCRBY
 *
 * ====================================================================
 * ARCHITECTURE: Three-Tier Design
 * ====================================================================
 *
 * Small Tier (< ~50 entries, configurable):
 *   - Single flex storing [score, member, score, member, ...]
 *   - Member lookup: O(n) linear scan
 *   - Score ops: O(log n) binary search
 *   - Memory: minimal overhead (~2-16 bytes header)
 *
 * Medium Tier (~50-200 entries):
 *   - Split flex array with index
 *   - Member lookup: O(n/k) where k = number of splits
 *   - Score ops: O(log n) with index acceleration
 *   - Memory: ~100-200 bytes overhead
 *
 * Full Tier (200+ entries):
 *   - Dual structure:
 *     1. memberIndex: multidict hash table (member → score)
 *     2. scoreMap: sorted flex array (score, member pairs)
 *   - Member lookup: O(1) hash table
 *   - Score/rank ops: O(log n) or O(n) depending on operation
 *   - Memory: ~64-80 bytes fixed + hash table overhead
 *
 * Tier promotion is automatic and transparent. No manual management.
 *
 * ====================================================================
 * PERFORMANCE CHARACTERISTICS (10,000 entries benchmark)
 * ====================================================================
 *
 * Operation               multiOrderedSet     multimap        Notes
 * -----------------------------------------------------------------------
 * Insert                  ~13,000/s           ~300,000/s      24x slower
 * Exists (by member)      ~2,200,000/s        N/A             O(1) hash
 * GetScore (by member)    ~2,600,000/s        N/A             O(1) hash
 * Lookup (by score/key)   N/A                 ~500,000/s      O(log n)
 * Random member lookup    ~2,000,000/s        ~450,000/s      4.5x faster
 * Iteration               ~7,400,000/s        ~7,100,000/s    Similar
 *
 * The key advantage: multimap cannot efficiently answer "what is member
 * X's score?" without O(n) scan. multiOrderedSet answers in O(1).
 *
 * ====================================================================
 * MEMORY USAGE (10,000 entries, ~6-byte keys)
 * ====================================================================
 *
 * multiOrderedSet (Full tier): ~412 KB (~41 bytes/entry)
 *   - memberIndex hash table: ~250 KB
 *   - scoreMap flex array: ~130 KB
 *   - Metadata/overhead: ~32 KB
 *
 * multimap equivalent: ~129 KB (~13 bytes/entry)
 *   - 3.2x less memory than multiOrderedSet
 *
 * Memory vs Speed trade-off:
 *   - multiOrderedSet uses 3x more memory for 4-5x faster member lookups
 *   - Justified when member-based queries are frequent
 *   - Not justified for write-heavy workloads or memory-constrained systems
 *
 * ====================================================================
 * COMPLEXITY SUMMARY
 * ====================================================================
 *
 * Operation                    Small       Medium      Full
 * -----------------------------------------------------------------------
 * Add/Update                   O(n)        O(n/k)      O(1) + O(log n)
 * Remove                       O(n)        O(n/k)      O(1) + O(n)
 * Exists (by member)           O(n)        O(n/k)      O(1)
 * GetScore (by member)         O(n)        O(n/k)      O(1)
 * GetRank (by member)          O(n)        O(n)        O(1) + O(n)
 * GetByRank                    O(n)        O(n)        O(n)
 * CountByScore                 O(log n)    O(log n)    O(log n) + O(m)
 * Iteration                    O(1)/next   O(1)/next   O(1)/next
 *
 * Where n = total entries, k = split factor, m = entries in range
 *
 * ====================================================================
 * DATA STRUCTURE CHOICES & RATIONALE
 * ====================================================================
 *
 * The Full tier uses three different data structures, each chosen for
 * specific performance characteristics:
 *
 * 1. multidict (hash table) for memberIndex
 *    ─────────────────────────────────────────
 *    Purpose: O(1) member → score lookups
 *
 *    Why multidict:
 *    - Hash table provides O(1) average-case lookup by member
 *    - Incremental rehashing prevents latency spikes during growth
 *    - Supports any databox type as key (strings, integers, etc.)
 *    - Memory-efficient slot packing vs naive hash table
 *
 *    Alternative considered: Skip list
 *    - Rejected: O(log n) lookup slower than O(1) hash
 *    - Skip list better for range queries, not point lookups
 *
 *    Memory cost: ~25 bytes/entry (hash overhead + score storage)
 *
 * 2. multiarray for scoreMap container
 *    ─────────────────────────────────────────
 *    Purpose: Hold multiple flex sub-maps with O(1) index access
 *
 *    Why multiarray:
 *    - O(1) access to any sub-map by index
 *    - Automatic growth without pointer invalidation
 *    - Cache-friendly contiguous storage of flex pointers
 *    - Simpler than managing raw pointer array with realloc
 *
 *    Alternative considered: Single large flex
 *    - Rejected: Binary search in huge flex is slower than
 *      index lookup + smaller flex search
 *    - Splitting allows parallel iteration in future
 *
 *    Memory cost: ~8 bytes per sub-map (pointer storage)
 *
 * 3. flex for sorted (score, member) pairs
 *    ─────────────────────────────────────────
 *    Purpose: Memory-efficient sorted storage with fast iteration
 *
 *    Why flex:
 *    - Variable-length encoding: small integers use 1-2 bytes
 *    - Cache-friendly: entries packed contiguously
 *    - No per-entry allocation overhead (vs linked structures)
 *    - O(log n) binary search for score-based operations
 *    - O(1) iteration via pointer arithmetic
 *
 *    Alternative considered: Red-black tree / AVL tree
 *    - Rejected: 24+ bytes overhead per node (pointers + color)
 *    - Tree traversal has poor cache locality
 *    - flex uses ~2-10 bytes per entry depending on value sizes
 *
 *    Alternative considered: Skip list
 *    - Rejected: High memory overhead (~32 bytes/entry for levels)
 *    - Random access pattern = cache misses
 *
 *    Memory cost: ~8-15 bytes/entry (variable encoding)
 *
 * WHY THE DUAL STRUCTURE?
 * ─────────────────────────────────────────
 *
 * The core problem: sorted sets need BOTH:
 *   A) Fast member lookup: "Does user X exist? What's their score?"
 *   B) Sorted iteration: "Top 10 users by score"
 *
 * Single-structure approaches fail:
 *   - Hash table: O(1) member lookup, but no sort order
 *   - Sorted array: O(log n) score ops, but O(n) member lookup
 *   - Skip list: O(log n) for both, but high memory overhead
 *
 * Dual structure solution:
 *   - memberIndex (hash): O(1) for member queries
 *   - scoreMap (sorted flex): O(log n) for score queries, O(1) iteration
 *   - Trade-off: 2x storage, but optimal time complexity for both
 *
 * This matches Redis ZSET design (dict + skiplist), but uses
 * more memory-efficient underlying structures (multidict + flex).
 *
 * TIER PROMOTION STRATEGY
 * ─────────────────────────────────────────
 *
 * Small → Medium: When flex exceeds size limit (~4KB default)
 *   - Splits single flex into indexed array of smaller flexes
 *   - Improves binary search by reducing search space per flex
 *
 * Medium → Full: When total entries exceed threshold (~200 default)
 *   - Adds hash table for O(1) member lookups
 *   - Significant memory increase, but necessary for performance
 *   - Threshold tunable via flexCapSizeLimit parameter
 *
 * Demotion (shrinking) is NOT implemented:
 *   - Simplifies code, avoids hysteresis oscillation
 *   - Most workloads grow monotonically or stay stable
 *   - Use multiOrderedSetReset() to manually shrink if needed
 *
 * ==================================================================== */

typedef struct multiOrderedSet multiOrderedSet;

/* ====================================================================
 * Score Range Specification
 * ==================================================================== */
typedef struct mosRangeSpec {
    databox min;
    databox max;
    bool minExclusive; /* true if min is exclusive (>) rather than (>=) */
    bool maxExclusive; /* true if max is exclusive (<) rather than (<=) */
} mosRangeSpec;

/* ====================================================================
 * Iterator
 * ==================================================================== */
typedef struct mosIterator {
    void *mos;         /* pointer to multiOrderedSet (untagged) */
    void *current;     /* current position (implementation-specific) */
    uint32_t mapIndex; /* for Medium/Full: which sub-map */
    uint32_t type : 2; /* tier type (1=Small, 2=Medium, 3=Full) */
    uint32_t forward : 1;
    uint32_t valid : 1; /* is iterator still valid? */
    uint32_t unused : 28;
} mosIterator;

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */

/* Create a new empty multiOrderedSet */
multiOrderedSet *multiOrderedSetNew(void);

/* Create with specific size limit for tier promotion */
multiOrderedSet *multiOrderedSetNewLimit(flexCapSizeLimit limit);

/* Create with compression enabled for deeper tiers */
multiOrderedSet *multiOrderedSetNewCompress(flexCapSizeLimit limit);

/* Duplicate an existing multiOrderedSet */
multiOrderedSet *multiOrderedSetCopy(const multiOrderedSet *mos);

/* Free a multiOrderedSet */
void multiOrderedSetFree(multiOrderedSet *mos);

/* Reset to empty state without deallocating */
void multiOrderedSetReset(multiOrderedSet *mos);

/* ====================================================================
 * Basic Statistics
 * ==================================================================== */

/* Number of (score, member) pairs */
size_t multiOrderedSetCount(const multiOrderedSet *mos);

/* Total bytes used */
size_t multiOrderedSetBytes(const multiOrderedSet *mos);

/* ====================================================================
 * Insertion / Update
 * ==================================================================== */

/* Add or update member with score.
 * Returns true if member already existed (score was updated).
 * Returns false if member was newly added. */
bool multiOrderedSetAdd(multiOrderedSet **mos, const databox *score,
                        const databox *member);

/* Add only if member does not exist (NX semantics).
 * Returns true if member was added, false if already existed. */
bool multiOrderedSetAddNX(multiOrderedSet **mos, const databox *score,
                          const databox *member);

/* Update only if member exists (XX semantics).
 * Returns true if member was updated, false if did not exist. */
bool multiOrderedSetAddXX(multiOrderedSet **mos, const databox *score,
                          const databox *member);

/* Add member with score, optionally getting the previous score.
 * Returns true if member already existed. */
bool multiOrderedSetAddGetPrevious(multiOrderedSet **mos, const databox *score,
                                   const databox *member, databox *prevScore);

/* Increment score of member by delta (numeric scores only).
 * If member doesn't exist, adds with delta as score.
 * Returns true on success, stores new score in result. */
bool multiOrderedSetIncrBy(multiOrderedSet **mos, const databox *delta,
                           const databox *member, databox *result);

/* ====================================================================
 * Deletion
 * ==================================================================== */

/* Remove member from set.
 * Returns true if member existed and was removed. */
bool multiOrderedSetRemove(multiOrderedSet **mos, const databox *member);

/* Remove member and return its score.
 * Returns true if member existed. */
bool multiOrderedSetRemoveGetScore(multiOrderedSet **mos, const databox *member,
                                   databox *score);

/* Remove members by score range.
 * Returns count of members removed. */
size_t multiOrderedSetRemoveRangeByScore(multiOrderedSet **mos,
                                         const mosRangeSpec *range);

/* Remove members by rank range (0-based, inclusive).
 * Negative ranks count from end (-1 = last).
 * Returns count of members removed. */
size_t multiOrderedSetRemoveRangeByRank(multiOrderedSet **mos, int64_t start,
                                        int64_t stop);

/* Pop member(s) with lowest score.
 * Returns count of members popped (up to count).
 * members and scores arrays must have space for 'count' elements. */
size_t multiOrderedSetPopMin(multiOrderedSet **mos, size_t count,
                             databox *members, databox *scores);

/* Pop member(s) with highest score.
 * Returns count of members popped (up to count).
 * members and scores arrays must have space for 'count' elements. */
size_t multiOrderedSetPopMax(multiOrderedSet **mos, size_t count,
                             databox *members, databox *scores);

/* ====================================================================
 * Lookup
 * ==================================================================== */

/* Check if member exists */
bool multiOrderedSetExists(const multiOrderedSet *mos, const databox *member);

/* Get score of member.
 * Returns true if member exists. */
bool multiOrderedSetGetScore(const multiOrderedSet *mos, const databox *member,
                             databox *score);

/* Get rank of member (0-based, by ascending score).
 * Returns -1 if member does not exist. */
int64_t multiOrderedSetGetRank(const multiOrderedSet *mos,
                               const databox *member);

/* Get reverse rank of member (0-based, by descending score).
 * Returns -1 if member does not exist. */
int64_t multiOrderedSetGetReverseRank(const multiOrderedSet *mos,
                                      const databox *member);

/* Get member at rank (0-based).
 * Negative rank counts from end (-1 = last).
 * Returns true if rank is valid. */
bool multiOrderedSetGetByRank(const multiOrderedSet *mos, int64_t rank,
                              databox *member, databox *score);

/* ====================================================================
 * Range Queries
 * ==================================================================== */

/* Count members in score range */
size_t multiOrderedSetCountByScore(const multiOrderedSet *mos,
                                   const mosRangeSpec *range);

/* ====================================================================
 * Iteration
 * ==================================================================== */

/* Initialize iterator at beginning (lowest score) */
void multiOrderedSetIteratorInit(const multiOrderedSet *mos, mosIterator *iter,
                                 bool forward);

/* Initialize iterator at specific score (or first entry >= score) */
bool multiOrderedSetIteratorInitAtScore(const multiOrderedSet *mos,
                                        mosIterator *iter, const databox *score,
                                        bool forward);

/* Initialize iterator at specific rank */
bool multiOrderedSetIteratorInitAtRank(const multiOrderedSet *mos,
                                       mosIterator *iter, int64_t rank,
                                       bool forward);

/* Get next (score, member) pair.
 * Returns false when iteration complete. */
bool multiOrderedSetIteratorNext(mosIterator *iter, databox *member,
                                 databox *score);

/* Release iterator resources (if any) */
void multiOrderedSetIteratorRelease(mosIterator *iter);

/* ====================================================================
 * Set Operations
 * ==================================================================== */

/* Aggregate function for combining scores */
typedef enum mosAggregate {
    MOS_AGGREGATE_SUM = 0,
    MOS_AGGREGATE_MIN = 1,
    MOS_AGGREGATE_MAX = 2
} mosAggregate;

/* Create union of sets. Caller must free result.
 * weights can be NULL for all 1.0 weights (double array).
 * numSets must be >= 1. */
multiOrderedSet *multiOrderedSetUnion(const multiOrderedSet *sets[],
                                      const double *weights, size_t numSets,
                                      mosAggregate aggregate);

/* Create intersection of sets. Caller must free result.
 * weights can be NULL for all 1.0 weights.
 * numSets must be >= 2. */
multiOrderedSet *multiOrderedSetIntersect(const multiOrderedSet *sets[],
                                          const double *weights, size_t numSets,
                                          mosAggregate aggregate);

/* Create difference (first - rest). Caller must free result. */
multiOrderedSet *multiOrderedSetDifference(const multiOrderedSet *sets[],
                                           size_t numSets);

/* ====================================================================
 * Random Access
 * ==================================================================== */

/* Get random member(s).
 * If count > 0, returns distinct members.
 * If count < 0, may return duplicates (abs(count) members).
 * Returns actual count retrieved. */
size_t multiOrderedSetRandomMembers(const multiOrderedSet *mos, int64_t count,
                                    databox *members, databox *scores);

/* ====================================================================
 * First / Last Access
 * ==================================================================== */

/* Get first (lowest score) entry without removing */
bool multiOrderedSetFirst(const multiOrderedSet *mos, databox *member,
                          databox *score);

/* Get last (highest score) entry without removing */
bool multiOrderedSetLast(const multiOrderedSet *mos, databox *member,
                         databox *score);

/* ====================================================================
 * Debugging / Testing
 * ==================================================================== */

#ifdef DATAKIT_TEST
void multiOrderedSetRepr(const multiOrderedSet *mos);
int multiOrderedSetTest(int argc, char *argv[]);
#endif
