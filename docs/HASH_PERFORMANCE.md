# Hash / envelope performance profile

Reference numbers for hash and envelope throughput on the supported
build configurations. Use these as a regression baseline.

All numbers below measured with the **`perf` preset** (Release -O3
-mcpu=native -flto, no sanitizers) — defined in `CMakePresets.json` and
described in [development/BUILDING.md](development/BUILDING.md).
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
| xxh3_64 / xxh3_128 envelope  | ~34 GB/s   | memory-bandwidth bound                       |
| BLAKE3 portable (public API) | ~1.37 GB/s | NEON Compress4 dispatch under the hood       |
| BLAKE3 NEON Compress4 (raw)  | ~1.57 GB/s | 4-chunk batched compress                     |
| BLAKE3 NEON single-block     | ~372 MiB/s | per-block compress (not batch — much slower) |
| SHA-256 ARMv8 crypto         | ~2.0 GB/s  | sha256h/h2/su0/su1 hardware                  |
| SHA-256 portable scalar      | not measured | see note below                             |
| HMAC-SHA-256 envelope        | ~2.0 GB/s  | inherits SHA-256 dispatch automatically      |
| HMAC-BLAKE3 envelope         | ~1.40 GB/s | inherits BLAKE3 SIMD dispatch                |
| BLAKE3_KEYED envelope        | ~1.40 GB/s | native keyed-hash mode                       |

Last re-measured on an Apple M1 Max (macOS 14.8, clang 22 `perf` preset)
with the commands above. The SHA-256 portable row has no current figure:
the `sha256ArmCrypto` bench's "portable" line times the public
`sha256Update()`, which dispatches to the ARMv8 Crypto path on this
hardware, so it does not measure the scalar fallback.

## Small-payload numbers (latency-dominated, 64-byte payload)

For RPC / cluster mailbox / replication-frame use cases:

| Path                                  | Per-op latency | Notes                                        |
| ------------------------------------- | -------------- | -------------------------------------------- |
| xxh64 envelope                        | wrap ~21 ns, verify ~31 ns | flexEnvelope bench                |
| HMAC-SHA-256 envelope (plain key)     | verify ~417 ns | dominated by 2 setup block compressions      |
| HMAC-SHA-256 envelope (prepared key)  | verify ~192 ns | setup blocks pre-absorbed, 2.2x faster       |
| HMAC-SHA-256 raw (plain hmacCompute)  | 386 ns/op      | hmac test bench                              |
| HMAC-SHA-256 raw (hmacComputeWithKey) | **164 ns/op**  | **2.36x speedup** for high-throughput verify |

Envelope figures are from perfRegression and the flexEnvelope bench, raw
HMAC from the hmac bench, all re-measured on the machine above.

## What the speedups came from

Each change and the gain measured when it landed. These are historical
figures; they do not match today's numbers, which are in the tables above.
For example, today the dev preset runs perfRegression's rows 1.0-7.7x
slower than `perf`, and 64-byte raw HMAC takes 386 → 164 ns/op.

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

Not implemented:

- **AVX2 Compress8 transpose** — x86 only. Expected ~20% over
  current AVX2 single-block. Requires an x86 test host.
- **BLAKE3 portable hot-path tuning** — diminishing returns on
  NEON-capable hosts since SIMD dispatch dominates. Matters on hosts
  without NEON/SSE4.1/AVX2 (rare in modern fleets).
- **Single-chunk BLAKE3 SIMD finalize** — for ≤1024-byte inputs
  the bulk Compress4 path doesn't engage; could batch parents.
- **Canonical BLAKE3 test-vector hex** — correctness, not perf.

## Hardware-ceiling notes

- `xxh3_*`: measured ~34 GB/s at 1 MiB, where it is limited by memory
  bandwidth; it only goes faster when the data is hot in cache.
- `sha256` ARM crypto: measured ~2.0 GB/s single-stream with
  `sha256h/sha256h2`. The 4-lane multi-buffer path (`sha256 multi4` in
  perfRegression) measured ~2.9 GB/s aggregate across its four messages.
- `blake3` NEON: measured ~1.57 GB/s for 4-way Compress4 on ARMv8.0 NEON;
  wider vectors (e.g. SVE2) would be needed to go further.
- `hmac`: the HMAC framing forces 2 extra block compressions per call.
  For 64-byte SHA-256 messages that measured ~220 ns per call (386 ns
  plain vs 164 ns prepared); the prepared-key path removes it for
  repeated-key workloads.

## CI-suitable build

For CI strict-correctness runs, use the `safe` preset (Release-O3 with
ASAN + UBSAN enabled):

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
