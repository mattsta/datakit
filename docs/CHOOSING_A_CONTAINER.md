# Choosing a Container

datakit's containers trade memory against compute on purpose: most of
them hold their data in flex byte arrays, packed without per-element
pointers, so they use far less memory than pointer-based structures and
pay for it in byte shifting and in-array searches. This guide says which
container fits which job, and what that costs in operations per second
and bytes per element.

Every figure here comes from [PERFORMANCE_MATRIX.md](PERFORMANCE_MATRIX.md),
which `dktools matrix` regenerates on any machine (see
[Measuring it yourself](#measuring-it-yourself)). The figures were taken
on one machine, an Apple M1 Max with clang 22 and the `perf` preset. They
are single-threaded, with keys in a shuffled order. Read them as orders
of magnitude and as ratios between containers, not as guarantees.

## At a glance

Throughput spans 1K to 1M elements and every element kind measured.

| You need                                              | Use             | Operations per second                                     | Bytes per element                                    |
| ----------------------------------------------------- | --------------- | --------------------------------------------------------- | ---------------------------------------------------- |
| Values by position, fixed width                       | multiarray      | 3-30M; reads to 190M while small                          | exactly the width                                    |
| A queue or log: append, pop, iterate                  | multilist       | 3-17M appends and pops                                    | 4-5 for small integers, width + 2 for strings        |
| Key -> value, lookups only                            | multidict       | 1.2-16M reads, 0.6-4.5M writes                            | key + value + 6-11                                   |
| Key -> value in key order (ranges, iteration)         | multimap        | 0.5-3M, every operation                                   | key + value + 4-9 (the densest map)                  |
| Members ranked by score                               | multiOrderedSet | 1.8-14M reads, 0.3-2M writes                              | member twice + score + ~20; over 40 bytes once + ~60 |
| Integer membership, read-mostly                       | intset          | 20-72M reads                                              | 2, 4 or 8, by the widest value                       |
| Integer membership, write-heavy or large              | multiroar       | 2.7-7M sets, 37-56M reads (clustered values)              | under 1 when values cluster                          |
| Many copies of the same values (interning)            | multimapAtom    | 1-2M interns (compact), 2-14M (fast); reads by id to 200M | value + 11-19 (compact), + 36-50 (fast)              |
| Recency for a cache stored elsewhere                  | multilru        | 16-92M                                                    | 14-23 per entry, no values                           |
| A sorted map read far more than written               | coldmap         | 0.5-2M lookups, 50M rows scanned                          | records at about a fifth of a multimap's             |
| A cache that keeps cold rows instead of dropping them | multitier       | the hot path is multidict's; a cold read about 0.1M       | hot rows as multidict, the rest as coldmap           |

## What the measurements show

### Sequences: multiarray and multilist

**multiarray** stores fixed-width elements with no per-element overhead:
8.0 bytes per 8-byte integer, 16.0 per 16-byte string. Up to 100K
elements it is the fastest of the containers that hold values at most
phases. Index reads of integers run at about 190M/s at 1K elements and
about 30M/s at 100K. At 1M elements the Large variant's node walk brings
every phase to about 3-5M/s.

**multilist** packs variable-width values, so small integers take 4-5
bytes rather than 8. Appending at the tail and popping it run at 7-17M/s
for integers and 16-byte strings at every size (3-8M/s for 256-byte
strings). Reading by index walks the nodes, so it slows as the list
grows: 2-6M/s at 1K, about 2M/s at 100K, and 0.17-0.6M/s at 1M. Walk a
multilist with its iterator rather than indexing into it (see
[PERFORMANCE.md](advanced/PERFORMANCE.md#iterator-vs-index-access)).

### Maps: multimap and multidict

**multimap** keeps its rows sorted in flex arrays. It is the densest map:
a row of two 8-byte integers takes 20 bytes, and a row of two 16-byte
strings takes 36. Every operation runs at 1-3M/s for integers and 16-byte
strings up to 100K rows, and at 0.5-1M/s for 256-byte strings at 100K
and 16-byte strings at 1M. Rows come back in key order, and ranges and
iteration are cheap.

**multidict** hashes its keys into flex-backed slots. For the same rows
it uses 10-30% more bytes than multimap for integers, 5-8% more for
16-byte strings, and under 1% more for 256-byte strings. Its reads run
1.4-14x faster than multimap's, 5.5-16M/s for integer keys. The gap is
widest for small integer keys and narrowest for 256-byte strings. Writes
run 1.5-4.5M/s for integers and 16-byte strings. Choose it when lookups
dominate and key order does not matter.

Small integers show the density trade inside multimap. Rows of small
integers pack 2-3 bytes per field, so more rows share each flex, and each
search scans further inside it. At 1K rows, small-integer creates run at
about 1M/s, against 2M/s for 8-byte integers. The per-flex size limit is
the knob for this trade; `dktools tune` measures it per machine
(tools/README.md, "tune").

### Ordered by score: multiOrderedSet

Looking up a member's score runs at 1.8-14M/s. Adding a member, or
changing its score, keeps two orders in step and runs at 0.3-2M/s. The
set keeps each member in both orders, by score and by member. A member of
40 bytes or less is stored in both, so its bytes count twice (a 16-byte
member takes about 58 bytes); a longer one is stored once, in the set's
atom table, with a reference in each order (a 256-byte member takes about
315 bytes).

### Integer sets: intset and multiroar

**intset** is one sorted array whose width (2, 4 or 8 bytes) follows the
widest value it holds. It is the densest exact integer set and reads at
20-72M/s. An insert or delete shifts the array behind it, so writes slow
as the set grows: 8-15M/s at 1K, 0.24-0.74M/s at 100K, and 20-45K/s at
1M. Use it for sets that are built once and then read, or that stay
below about 10K members.

**multiroar** is a roaring bitmap. For clustered values (the matrix's
small integers) it takes under one byte per member at 100K and more.
Sets run at 2.7-7M/s and reads at 37-56M/s. Values spread across the
whole 64-bit range each land in a container of their own: about 16 bytes
and 1.5-2.9M operations/s per member. For scattered values use intset
(read-mostly) or multidict.

### Values shared by many owners: multimapAtom

multimapAtom interns values (strings, integers, any databox): each
distinct value is held once and named by an id, with a count of the
holders (see [ATOMS.md](modules/multi/ATOMS.md)). The matrix measures two
tables. The default, compact one (a hashed index over packed rows) holds
an atom in its value's bytes plus 11-19: 275 bytes for a 256-byte string.
It interns at 1-2M/s and reads by id at 4.5-35M/s. The fast one (a slot
index over one row per id) interns at 2-14M/s and reads by id at
70-200M/s for integers and 5-14M/s for strings, for 25-30 more bytes per
atom than the compact table.

### Cache recency: multilru

multilru keeps only recency and weights per handle, at 14-23 bytes each.
It stores no values, so it pairs with a container that does. Promotions,
weight updates and deletes run at 16-92M/s, and weight updates reach
440M/s while small.

### Read-mostly and tiered maps: coldmap and multitier

These two are measured by perfRegression (`coldmap`, `coldmapLimits`,
`multitier`), not the matrix. coldmap keeps sorted rows in LZ4-compressed
blocks: 100,000 record rows take 1.27 MB at the default 8 KB blocks
against a multimap's 6 MB, with 2 us lookups (0.55 us at 1 KB blocks for
2.48 MB) and 19 ns a row to scan; single-row writes cost about 5 us, sorted
batches one recompression per block. multitier puts a multidict in front of
a coldmap and a sink behind it: held to a quarter of the rows hot, the
same 100,000 rows take 2.79 MB against 6.04 MB, reads of the hot tenth stay
fast and a read from the cold level costs about 9 us. See
[COLDMAP.md](modules/multi/COLDMAP.md) and
[MULTITIER.md](modules/multi/MULTITIER.md).

## Measuring it yourself

```sh
uv run --project tools dktools matrix NAME --runs 3          # report under build/dktools/matrix/NAME
uv run --project tools dktools matrix NAME --runs 3 --write  # also rewrite docs/PERFORMANCE_MATRIX.md
```

The measurement is the `containerMatrix` test (src/containerMatrix.c).
Run as a plain test (`datakit-test test containerMatrix`), it measures
100 elements of each kind and checks every result. `DK_MATRIX=full`
measures 1K, 100K and 1M elements, with 256-byte strings stopping at
100K. A new container joins the matrix as one driver in that file: its
make, create, read, update, delete, bytes and count functions, plus the
element kinds and largest size it takes.

## See also

- [Scale-Aware Design](advanced/SCALE_AWARE.md): the size variants
  inside each container family
- [Memory Patterns](advanced/MEMORY.md): where the bytes go
- [Performance Guide](advanced/PERFORMANCE.md): getting the most out of
  each container
