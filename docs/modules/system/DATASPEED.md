# dataspeed: measuring the machine

**Header**: `dataspeed.h` **Source**: `dataspeed.c` **Command**: `datakit-test speed [--json|--csv]`

dataspeed measures the machine it runs on: cache and memory bandwidth, memory latency, the cache sizes the
latency curve shows, and CPU operation throughput. Use it to compare machines, builds and settings (a
new server type, huge pages on or off, a compiler change), and to record the hardware next to benchmark
results. The numbers move with load, frequency scaling and temperature: compare runs from the same
conditions.

## Running it

```sh
datakit-test speed            # progress, then the report table
datakit-test speed --json     # the report as JSON only (for tooling)
datakit-test speed --csv      # metric,value,unit rows
datakit-test bench dataspeed  # the self-checks, with short measurements
```

From C:

```c
#include "dataspeed.h"

dataspeedConfig config = dataspeedDefaultConfig();
config.verbose = false;          /* no progress output */
config.min_duration_ms = 250;    /* steadier numbers, longer run */

dataspeedReport report;
dataspeedRunWithConfig(&report, &config);
dataspeedPrintReportJSON(&report);
```

`dataspeedRunAll(&report, verbose)` is the same with the defaults.

## Configuration

| Field               | Default | Effect                                                                        |
| ------------------- | ------- | ----------------------------------------------------------------------------- |
| `min_duration_ms`   | 100     | each measurement runs at least this long                                      |
| `max_iterations`    | 10^9    | cap on any calibrated loop's iteration count                                  |
| `warmup_iterations` | 3       | untimed runs of each bandwidth pass and latency sample before timing          |
| `memory_mb`         | 0       | the memory bandwidth working set; 0 sizes it past the largest cache (≥ 64 MB) |
| `verbose`           | true    | print each result as it is measured                                           |
| `include_latency`   | true    | run the latency tests and the cache sweep (the slowest part)                  |

A NULL config anywhere means the defaults.

## What it measures

| Part                                                | How                                                                                                                                                                  |
| --------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `dataspeedGetSystemInfo`                            | CPU model and count, an estimated frequency, cache sizes and line size (sysctl on macOS, sysfs on Linux), memory and page size; `dataspeedPrintSystemInfo` prints it |
| `dataspeedBenchmarkCacheHierarchy(&r, system, cfg)` | read bandwidth with the working set in L1, L2, L3 and RAM; with latency on, a sweep from 4 KB up whose latency steps give `detected_l1/2/3_size`                     |
| `dataspeedBenchmarkLatency(&r, system, cfg)`        | load-to-load latency through a randomly ordered pointer chain in each level's working set                                                                            |
| `dataspeedBenchmarkMemory(&r, size_mb, cfg)`        | sequential read, write (memset) and copy (memcpy) bandwidth                                                                                                          |
| `dataspeedBenchmarkCPU(&r, cfg)`                    | dependent-chain throughput of integer and float add, multiply and divide and FMA; the cost of a call; a branch over a predictable and a random pattern               |

Working sets come from the reported caches: half of each level, at least twice the level below (twice the
L2 when no L3 is reported), and four times the largest cache, at least 64 MB, for RAM. Each result records
the working set it used. Bandwidth and latency report the median of their samples, with min, max, mean,
standard deviation and 95th percentile in `stats`.

The detected sizes are the largest working set before each step up in latency (a rise of 30% or more; a
run of rises on consecutive sizes is one step). They are estimates: the reported sizes are what the OS
says, and the detected ones are what the access pattern sees (shared caches and prefetchers move them).

## Reading the results

- The CPU loops are compiled without optimization, so every operation goes through memory. Their numbers
  compare machines and builds; they are not the instruction throughput of optimized code.
- `branch_random_ns - branch_predictable_ns` is about half a misprediction's cost (half the random
  branches are mispredicted).
- `cycles` and `cycles_per_byte` count the timestamp counter (TSC on x86, CNTVCT on ARM), which may tick
  at a fixed rate unrelated to the core clock.
- Run the perf build (`cmake --preset perf`) for meaningful numbers: sanitizer builds are several times
  slower.

`dataspeedPrintReport`, `dataspeedPrintReportCSV` and `dataspeedPrintReportJSON` print a report.
`dataspeed(MB, iterations)` is the command-line run: every benchmark with progress and the report, then,
when both are positive, `iterations` memsets of `MB` megabytes with running throughput (Ctrl-C stops them).
