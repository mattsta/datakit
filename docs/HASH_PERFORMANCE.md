# Hash / envelope performance profile

Reference numbers for hash and envelope throughput on the supported
build configurations. Use these as a regression baseline.

All numbers below measured with the **`perf` preset** (Release -O3
-mcpu=native -flto, no sanitizers) — see [BUILD_PRESETS.md](BUILD_PRESETS.md).
On Apple Silicon (M-series), single-thread, 32 KiB working sets unless
noted.

To reproduce:

```sh
cmake --preset perf && cmake --build --preset perf
./build-perf/src/datakit-test test blake3Neon       # BLAKE3 + NEON paths
./build-perf/src/datakit-test test sha256ArmCrypto  # SHA-256 portable vs HW
./build-perf/src/datakit-test test hmac             # HMAC + prepared-key
./build-perf/src/datakit-test test flexEnvelope     # full envelope matrix
```

## Headline numbers (Apple Silicon, perf preset, 1 MiB payload)

| Path                         | Throughput | Notes                                        |
| ---------------------------- | ---------- | -------------------------------------------- |
| xxh64 envelope               | ~13 GB/s   | memory-bandwidth bound                       |
| xxh3_64 / xxh3_128 envelope  | ~28 GB/s   | memory-bandwidth bound                       |
| BLAKE3 portable (public API) | ~1.36 GB/s | NEON Compress4 dispatch under the hood       |
| BLAKE3 NEON Compress4 (raw)  | ~1.55 GB/s | 4-chunk batched compress                     |
| BLAKE3 NEON single-block     | ~415 MiB/s | per-block compress (not batch — much slower) |
| SHA-256 ARMv8 crypto         | ~2.0 GB/s  | sha256h/h2/su0/su1 hardware                  |
| SHA-256 portable scalar      | ~140 MiB/s | fallback when crypto extensions absent       |
| HMAC-SHA-256 envelope        | ~2.0 GB/s  | inherits SHA-256 dispatch automatically      |
| HMAC-BLAKE3 envelope         | ~1.36 GB/s | inherits BLAKE3 SIMD dispatch                |
| BLAKE3_KEYED envelope        | ~1.36 GB/s | native keyed-hash mode                       |

## Small-payload numbers (latency-dominated, 64-byte payload)

For RPC / cluster mailbox / replication-frame use cases:

| Path                                  | Per-op latency | Notes                                        |
| ------------------------------------- | -------------- | -------------------------------------------- |
| xxh64 envelope                        | ~75 ns         | wrap + verify each ~70 ns                    |
| HMAC-SHA-256 envelope (plain key)     | ~250 ns        | dominated by 2 setup block compressions      |
| HMAC-SHA-256 envelope (prepared key)  | ~190 ns        | setup blocks pre-absorbed, ~1.5x faster      |
| HMAC-SHA-256 raw (plain hmacCompute)  | 183 ns/op      | hmac test bench                              |
| HMAC-SHA-256 raw (hmacComputeWithKey) | **96 ns/op**   | **1.91x speedup** for high-throughput verify |

## What the speedups came from

The journey from where we started to current numbers, with the
single change that produced each gain:

| Step                                      | Effect                                             |
| ----------------------------------------- | -------------------------------------------------- |
| Sanitizer-free `perf` preset              | 8-18× across the board — sanitizers were the floor |
| Wire NEON Compress4 into BLAKE3 hasher    | BLAKE3 1.36 GB/s, was ~700 MiB/s sequential        |
| Re-trigger Compress4 fast path mid-Update | HMAC-BLAKE3 envelope 699 → 1361 MB/s (≈2×)         |
| LE memcpy for CV reload                   | ~3% per call inside the fast path                  |
| SHA-256 ARM-crypto runtime dispatch       | SHA-256 116 → 2034 MiB/s (17.5×) — HMAC inherits   |
| `-flto` in perf preset                    | +1-4% via cross-TU inlining                        |
| Prepared-key HMAC API                     | 64-byte HMAC 183 → 96 ns/op (1.91× for small msgs) |

## Where additional wins are possible

Not implemented in this iteration; tracked in the task list:

- **AVX2 Compress8 transpose** (P7.6) — x86 only. Expected ~20% over
  current AVX2 single-block. Requires x86 test host.
- **BLAKE3 portable hot-path tuning** (P6.2) — diminishing returns on
  NEON-capable hosts since SIMD dispatch dominates. Matters on hosts
  without NEON/SSE4.1/AVX2 (rare in modern fleets).
- **Single-chunk BLAKE3 SIMD finalize** (P7.3) — for ≤1024-byte inputs
  the bulk Compress4 path doesn't engage; could batch parents.
- **Canonical BLAKE3 test-vector hex** (P8.2) — correctness, not perf.

## Hardware-ceiling notes

- `xxh3_*`: bound by memory bandwidth at 28 GB/s. Single-core can't go
  faster unless the data is hot in L1.
- `sha256` ARM crypto: 2.0 GB/s is the documented Apple Silicon
  throughput for `sha256h/sha256h2`. Multi-block multi-buffer
  scheduling could in principle hit ~3 GB/s on the M-series but adds
  significant code complexity.
- `blake3` NEON: 1.55 GB/s for 4-way Compress4 is a known ceiling for
  ARMv8.0 NEON. ARMv9 SVE2 or wider vector widths could push this
  higher.
- `hmac`: the HMAC framing forces 2 extra block compressions per
  call. For SHA-256 on Apple Silicon, that's ~60 ns of fixed cost.
  Prepared-key bypass removes that for repeated-key workloads.

## CI-suitable build

For CI strict-correctness runs, use the `safe` preset (Release-O3 +
all sanitizers):

```sh
cmake --preset safe && cmake --build --preset safe
./build-safe/src/datakit-test test flex
./build-safe/src/datakit-test test blake3
./build-safe/src/datakit-test test flexEnvelope
# etc.
```

The `dev` preset is similar but is the recommended default for
day-to-day development. Use `debug` when you need a non-optimized
build with full debug symbols for stepping.
