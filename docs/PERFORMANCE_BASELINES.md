# Performance baselines ledger

Dated, machine-tagged benchmark captures so performance claims can be
compared over time and regressions argued from numbers, not memory.
Append a new dated section per capture; never rewrite old sections —
the point of this file is the time series.

Rules for a capture:

- **perf preset only** (`cmake --preset perf`: Release-O3,
  `-mcpu=native`, `-flto`, no sanitizers). Sanitizer builds add 3-10×
  overhead and invalidate every number.
- Record hardware, date, and the exact commands.
- If a number legitimately regresses (e.g. a security fix adds work),
  note the why here in the same section.

The row-level regression gate is `datakit-test test perfRegression`
compared against a locally kept TSV via `scripts/perfDiff.py`
(tolerance 10% routine / 5% release). This ledger keeps the headline
numbers and decisions; the TSV keeps every row.

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
seal/open ≈ 586. Full rows go to the TSV for `perfDiff.py`.

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
