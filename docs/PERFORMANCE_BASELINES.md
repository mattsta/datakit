# Performance baselines ledger

Dated, machine-tagged benchmark captures so performance claims can be
compared over time and regressions argued from numbers, not memory.
Append a new dated section per capture; never rewrite old sections —
the point of this file is the time series.

Rules for a capture:

- **perf preset only** (`cmake --preset perf`: Release-O3,
  `-mcpu=native`, `-flto`, no sanitizers). Sanitizer builds invalidate
  every number, unevenly. The `dev` preset ran perfRegression's rows
  1.0-7.7× slower than `perf` on an Apple M1 Max with clang 22: about 1.0×
  for memory-bandwidth-bound or hardware-instruction paths, the most for
  scalar code with many loads and stores per byte. See
  [development/TESTING.md](development/TESTING.md) for the comparison
  commands.
- Record hardware, date, and the exact commands.
- If a number legitimately regresses (e.g. a security fix adds work),
  note the why here in the same section.

The row-level regression gate is `datakit-test test perfRegression`
compared against a locally kept TSV with
`cd tools && uv run dktools perfdiff BASELINE.tsv CURRENT.tsv`
(tolerance 10% routine / 5% release, via `--tolerance`). The baseline
TSV stays local to each machine and is not committed. This ledger keeps
the headline numbers and decisions; the TSV keeps every row.

---

## 2026-10-21 — Fenwick and segment trees: workload matrix

Apple M1 Max, macOS 14.8, clang 22, `perf` preset, single thread. ns per
operation; `B/elem` includes the power-of-two capacity. Segment `update`
in the counter table is a Get plus an assigning Update. Interpretation and
the choosing guide: [modules/fenwick/FENWICK.md](modules/fenwick/FENWICK.md).

Changes measured here (same machine): `multiFenwickFlat` query 31.7 ->
16.5 ns and update 40.8 -> 27.0 ns at 1K, 49.6 -> 20.6 / 90.1 -> 36.9 ns
at 100K (lazy kinds, integer fast paths); LowerBound by binary lifting:
Flat 70.7 -> 26.7 ns at 1K, multiFenwick 30.0 -> 7.9 us; `multiFenwick2d`
on the shared adaptive store: update 63-189 -> 47-131 ns, 9.4 -> 8.4
B/cell.

### Counter table: integer counts, +1 updates

| tree             |       N |  build |  update |   query |    get |   range |   lower | B/elem |
| ---------------- | ------: | -----: | ------: | ------: | -----: | ------: | ------: | -----: |
| fenwickI16       |    1000 |  181.0 |    27.1 |    15.5 |   12.2 |    20.1 |    18.5 |   2.06 |
| fenwickI32       |    1000 |   28.0 |    27.0 |    17.5 |   12.4 |    24.8 |    14.7 |   4.10 |
| fenwickI64       |    1000 |   21.0 |    26.4 |    32.8 |   13.4 |    18.9 |    11.4 |   8.20 |
| fenwickI128      |    1000 |   50.0 |    28.2 |    15.7 |   12.5 |    20.8 |    20.9 |  16.40 |
| fenwickFloat     |    1000 |   38.0 |    26.3 |    15.5 |   12.1 |    18.9 |    18.3 |   4.10 |
| fenwickDouble    |    1000 |   12.0 |    26.2 |    14.4 |   12.0 |    20.1 |    16.4 |   8.20 |
| multiFenwickFlat |    1000 |  274.0 |    27.0 |    16.0 |   13.3 |    21.5 |    24.2 |   8.22 |
| multiFenwick     |    1000 | 3355.0 |  9207.2 |  4538.7 | 1677.9 |  8111.1 |  7746.3 |   3.19 |
| segmentI64 SUM   |    1000 |   52.0 |    22.3 |    39.6 |    1.5 |    38.4 |       - |  16.40 |
| fenwickI16       |  100000 |   10.1 |    33.0 |    19.5 |   12.2 |    24.7 |    30.5 |   2.62 |
| fenwickI32       |  100000 |    9.7 |    33.4 |    19.3 |   11.7 |    25.0 |    39.8 |   5.24 |
| fenwickI64       |  100000 |   10.4 |    31.3 |    18.8 |   11.8 |    24.2 |    47.3 |  10.49 |
| fenwickI128      |  100000 |   11.6 |    32.8 |    17.9 |   12.6 |    27.5 |    82.8 |  20.97 |
| fenwickFloat     |  100000 |   12.2 |    31.4 |    19.8 |   11.7 |    23.4 |    46.3 |   5.24 |
| fenwickDouble    |  100000 |   11.8 |    32.3 |    18.1 |   11.7 |    24.4 |    53.0 |  10.49 |
| multiFenwickFlat |  100000 |    5.4 |    37.4 |    19.1 |   12.8 |    25.8 |    76.2 |  10.49 |
| multiFenwick     |  100000 | 8657.9 | 32510.2 | 15910.2 | 4552.0 | 31130.2 | 32340.6 |   3.94 |
| segmentI64 SUM   |  100000 |   38.2 |    43.6 |    62.8 |    1.5 |    62.4 |       - |  20.97 |
| fenwickI16       | 4000000 |   11.9 |    43.7 |    21.9 |   12.2 |    31.8 |    89.5 |   2.10 |
| fenwickI32       | 4000000 |   12.0 |    47.1 |    24.2 |   12.4 |    39.9 |   232.2 |   4.19 |
| fenwickI64       | 4000000 |   12.2 |    49.7 |    26.9 |   12.2 |    49.2 |   327.4 |   8.39 |
| fenwickI128      | 4000000 |   14.2 |   105.8 |    45.8 |   13.6 |    79.1 |   557.0 |  16.78 |
| fenwickFloat     | 4000000 |   12.0 |    47.0 |    22.2 |   12.0 |    29.8 |   232.1 |   4.19 |
| fenwickDouble    | 4000000 |   14.0 |    60.1 |    23.0 |   12.0 |    36.4 |   322.3 |   8.39 |
| multiFenwickFlat | 4000000 |    4.5 |    74.4 |    37.5 |   14.4 |    64.7 |   454.6 |   8.39 |
| multiFenwick     | 4000000 | 9506.7 | 70102.9 | 34166.4 | 6245.5 | 63617.2 | 67082.0 |   3.16 |
| segmentI64 SUM   | 4000000 |   52.0 |   108.3 |    86.4 |    7.1 |    84.2 |       - |  16.78 |

(1K builds include first-call warm-up; read build at 100K and 4M.)

### Mixed values: 1% of values and updates are doubles

| tree              |       N |   build |  update |   query |    get |   range |   lower | B/elem |
| ----------------- | ------: | ------: | ------: | ------: | -----: | ------: | ------: | -----: |
| fenwickDouble     |    1000 |    13.0 |    26.3 |    14.7 |   11.6 |    18.9 |    15.8 |   8.20 |
| multiFenwickFlat  |    1000 |     7.0 |    46.2 |    26.2 |   13.7 |    28.8 |    95.1 |   9.25 |
| multiFenwick      |    1000 |  2781.0 | 10354.2 |  5032.0 | 1902.2 |  8864.1 |  8970.9 |   3.23 |
| segmentDouble SUM |    1000 |    37.0 |    35.5 |    39.3 |    1.5 |    39.6 |       - |  16.40 |
| fenwickDouble     |  100000 |    11.8 |    30.2 |    18.5 |   12.1 |    24.7 |    53.4 |  10.49 |
| multiFenwickFlat  |  100000 |     6.7 |    72.8 |    32.6 |   14.8 |    40.6 |   257.8 |  11.80 |
| multiFenwick      |  100000 |  9151.4 | 39393.6 | 18677.1 | 5106.7 | 36235.2 | 37473.4 |   3.99 |
| segmentDouble SUM |  100000 |    61.9 |    62.6 |    61.6 |    1.4 |    61.5 |       - |  20.97 |
| fenwickDouble     | 4000000 |    13.7 |    61.4 |    21.5 |   12.5 |    35.7 |   300.1 |   8.39 |
| multiFenwickFlat  | 4000000 |     5.8 |   157.9 |    54.7 |   21.1 |   133.3 |   650.1 |   9.44 |
| multiFenwick      | 4000000 | 10055.0 | 79703.1 | 39085.9 | 6928.8 | 68644.5 | 73656.2 |   3.20 |
| segmentDouble SUM | 4000000 |    84.4 |   123.4 |    85.2 |    6.8 |    87.8 |       - |  16.78 |

### Sparse growth: n writes at random indexes of an empty tree (ns per write)

| tree             |     1K |    100K |        4M | B/elem at 4M |
| ---------------- | -----: | ------: | --------: | -----------: |
| fenwickI64       |   30.0 |    34.7 |      49.8 |         8.39 |
| multiFenwickFlat |   30.0 |    38.4 |      85.9 |         8.39 |
| multiFenwick     | 9380.0 | 32811.8 | (skipped) | 3.94 at 100K |
| segmentI64 SUM   |   25.0 |    41.5 |     105.7 |        16.78 |

### Range minimum

| tree              |       N | build | update | query | get | range | B/elem |
| ----------------- | ------: | ----: | -----: | ----: | --: | ----: | -----: |
| segmentI64 MIN    |    1000 |  27.0 |   26.0 |  40.7 | 1.6 |  40.5 |  16.40 |
| segmentDouble MIN |    1000 |  44.0 |   43.0 |  39.1 | 1.5 |  38.2 |  16.40 |
| segmentI64 MIN    |  100000 |  42.9 |   45.8 |  61.9 | 1.4 |  64.1 |  20.97 |
| segmentDouble MIN |  100000 |  74.1 |   74.1 |  61.8 | 1.4 |  60.7 |  20.97 |
| segmentI64 MIN    | 4000000 |  59.0 |  118.9 |  86.6 | 7.4 |  96.2 |  16.78 |
| segmentDouble MIN | 4000000 |  96.4 |  131.6 |  84.4 | 6.4 |  83.9 |  16.78 |

### 2D grid: square grid of about N cells ('range' = rectangle sum)

| tree                       |       N | build | update | query |  get | range | B/elem |
| -------------------------- | ------: | ----: | -----: | ----: | ---: | ----: | -----: |
| fenwick2dI64               |    1000 |  43.7 |   62.9 |  20.2 | 19.2 |  34.7 |   8.57 |
| fenwick2dDouble            |    1000 |  39.5 |   57.7 |  20.9 | 19.9 |  34.6 |   8.57 |
| multiFenwick2d             |    1000 |  60.4 |   44.9 |  20.2 | 24.6 |  39.3 |   8.57 |
| multiFenwick2d, 1% doubles |    1000 |  81.2 |   86.4 |  26.5 | 21.4 |  53.3 |   9.64 |
| fenwick2dI64               |  100000 |  65.1 |  113.3 |  27.2 | 21.7 |  58.6 |  21.00 |
| fenwick2dDouble            |  100000 |  60.5 |  104.5 |  26.1 | 21.3 |  54.6 |  21.00 |
| multiFenwick2d             |  100000 |  58.2 |   97.9 |  26.5 | 26.3 |  62.7 |  21.00 |
| multiFenwick2d, 1% doubles |  100000 | 126.2 |  179.2 |  42.3 | 23.7 | 117.1 |  23.63 |
| fenwick2dI64               | 4000000 |  86.1 |  316.7 |  71.4 | 22.0 | 227.0 |   8.39 |
| fenwick2dDouble            | 4000000 |  75.7 |  221.1 |  57.0 | 23.6 | 188.5 |   8.39 |
| multiFenwick2d             | 4000000 |  79.8 |  212.9 |  92.1 | 47.4 | 284.1 |   8.39 |
| multiFenwick2d, 1% doubles | 4000000 | 141.5 |  314.5 | 162.6 | 62.1 | 503.4 |   9.44 |

(At 100K cells the grid is 316 x 316 in a 512 x 512 capacity, hence 21 B.)

### Reproduce

```
./build-perf/src/datakit-test test fenwickBench
```

---

## 2026-08-25 — timer subsystem overhaul (timerWheel + multiTimer)

| Field    | Value                                   |
| -------- | --------------------------------------- |
| CPU      | Apple M1 Max (arm64, ARMv8 Crypto/NEON) |
| Preset   | `perf`                                  |
| Compiler | Apple clang (system `cc`)               |

### timerWheel — hierarchical wheel after entry-cascade + occupancy-bitmap work

`./build-perf/src/datakit-test test timerWheel` (cycles per op, TSC):

| operation                 | cost        | note                                                                                                                                                        |
| ------------------------- | ----------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| register                  | 5.4–5.8 cy  | 3–3.6× faster than multiTimer                                                                                                                               |
| expiration (batch fire)   | ~1 cy       |                                                                                                                                                             |
| next-event query (cached) | 0.1 cy      | min-fold cache survives register                                                                                                                            |
| steady-state churn        | ~8 cy/iter  | reg+cancel+query mix (was 64 before the intset realloc fix below)                                                                                           |
| register-then-cancel 100k | 8.5 cy      | was 173 cy: tombstone intset Add/Remove now realloc+memmove in place (Medium/Full tiers previously alloc+double-copy+free per op); scaling is flat 10k→100k |
| 12h idle-gap catch-up     | ~1.5 ms     | was ~150 ms+ before slot hopping                                                                                                                            |
| memory                    | ~27 B/timer |                                                                                                                                                             |

### multiTimer — compact sorted-map scheduler after watermark-StopAll work

| operation  | cost    | note                            |
| ---------- | ------- | ------------------------------- |
| register   | ~22 cy  | O(log n) sorted insert          |
| expiration | ~1.2 cy |                                 |
| StopAll    | 417 ns  | at 50k lifetime ids (was O(n²)) |

### multiroar — bulk bit-set batching (same capture date)

`./build-perf/src/datakit-test test multiroar`, PERF section, 100k
sorted positions:

| operation           | cost        | note                          |
| ------------------- | ----------- | ----------------------------- |
| BitSet per-bit loop | 93.3 ns/bit | one map op per bit (old path) |
| BitSetMany batched  | 2.8 ns/bit  | one map op per chunk run, 33× |

BitGetMany also caches chunk-run lookups (no separate bench row; same
mechanism, read-only).

**Decisions this capture supports:**

1. Entry-cascade fires higher-wheel timers within 1 ms of schedule
   (previously up to a full higher-wheel slot late) at no measured
   register/fire cost.
2. timerWheel for latency-critical/churn-heavy timers; multiTimer for
   compact populations — both now share cancellation semantics.
3. Bulk multiroar loads (multiroarFromArray, BitSetMany) should pass
   sorted/clustered positions to hit the run-batched path.

### Reproduce

```
./build-perf/src/datakit-test test timerWheel
./build-perf/src/datakit-test test multiTimer
./build-perf/src/datakit-test test multiroar
```

---

## 2026-08-15 — allocators, intset search, crypto regression rows

| Field    | Value                                   |
| -------- | --------------------------------------- |
| CPU      | Apple M1 Max (arm64, ARMv8 Crypto/NEON) |
| Preset   | `perf`                                  |
| Compiler | Apple clang (system `cc`)               |

### intset binary search — branchless+prefetch integrated

`./build-perf/intsetSearchBench` (10M random probes, ns/lookup),
measuring the exact macro now used by the five width-specialized Find
functions (`INTSET_BRANCHLESS_LOWER_BOUND`):

| n         | branchy | branchless+pf | speedup |
| --------- | ------- | ------------- | ------- |
| 64        | 14.18   | 6.94          | 2.04×   |
| 1,024     | 22.19   | 12.39         | 1.79×   |
| 65,536    | 49.73   | 34.05         | 1.46×   |
| 1,048,576 | 93.07   | 70.21         | 1.33×   |
| 2,097,152 | 127.78  | 92.94         | 1.37×   |

### Allocators — membound vs memshard vs system

`./build-perf/memboundBench` (ns per alloc+free pair; live=1000).
memshard is the vendored allocator at `src/memshard/` (mimalloc
v2.3.2 base + v2.4.x correctness line).

Single-thread:

| allocator    | 64B   | 256B  | 1024B |
| ------------ | ----- | ----- | ----- |
| system       | 10.02 | 19.97 | 25.68 |
| memshard     | 2.41  | 3.24  | 4.82  |
| zmalloc      | 9.80  | 19.58 | 32.14 |
| membound-STD | 19.99 | 19.81 | 20.11 |
| membound-ENT | 19.26 | 19.20 | 19.45 |
| membound-TC  | 15.86 | 15.82 | 16.43 |

Multi-thread ×4, shared pool:

| allocator    | 64B   | 256B  |
| ------------ | ----- | ----- |
| system       | 5.65  | 11.91 |
| memshard     | 0.63  | 0.86  |
| zmalloc      | 5.26  | 13.07 |
| membound-STD | 67.82 | 67.26 |
| membound-ENT | 67.81 | 63.67 |
| membound-TC  | 29.21 | 28.57 |

With `MEMBOUND_BENCH_MEMSHARD=1` (routes the global zmalloc backend
through `datakitUseMemshard()`), the `zmalloc` row becomes 3.35 ns ST
/ 0.86 ns MT at 64B — roughly 3-6× single-thread and 6-9×
multi-thread on the global-allocator path.

Space axes (unchanged from prior captures): memshard worst-case waste
1.1× and churn footprint tracks live (~0.97×); membound-STD wastes up
to 32× on tiny objects (256B minimum atom) and pre-commits its pool.

**Decisions this capture supports:**

1. The membound thread-cache is a real ~2.3× multithread win over
   membound-STD, but membound remains ~46× behind memshard under
   contention — use membound for its semantics (bounded,
   process-shared, arena, compaction), never as a general allocator.
2. memshard is the recommended global backend for multithreaded
   workloads via the opt-in `datakitUseMemshard()`.
3. The branchless intset search speedup survived integration into the
   real Find paths at the same magnitude the decision bench promised.

### Crypto/envelope regression rows (spot headline)

`./build-perf/src/datakit-test test perfRegression`, MB/s at 1 MiB
payload: hmac_blake3 wrap ≈ 1388, verify ≈ 1424; ChaCha20-Poly1305
seal/open ≈ 586. Full rows go to the local TSV for `dktools perfdiff`.

### Reproduce

```
cmake --preset perf
cmake --build build-perf --target intsetSearchBench memboundBench datakit-test
./build-perf/intsetSearchBench
./build-perf/memboundBench
MEMBOUND_BENCH_MEMSHARD=1 ./build-perf/memboundBench
./build-perf/src/datakit-test test perfRegression
```

---

## 2026-05-17 — crypto/envelope baseline (pre-allocator work)

Apple M1 Max, perf preset. Headline rows retained for comparison:
blake3 1 MiB ≈ 1398 MB/s, sha256 1 MiB ≈ 1984 MB/s. Captured
immediately after the crypto/cluster/envelope stack landed; the full
88-row TSV from this capture is superseded by later regenerations.
