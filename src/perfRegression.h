#pragma once

/* perfRegression — single, CI-runnable benchmark suite covering every
 * critical hot path the security stack relies on.
 *
 * Goals:
 *   1. Deterministic workloads (fixed seeds, fixed sizes) so two runs
 *      on the same hardware produce comparable numbers.
 *   2. Stable, machine-parseable TSV output — one row per (module,
 *      test, payload_bytes) tuple.  CI diffs current vs. last-known-
 *      good and flags regressions > a fixed threshold.
 *   3. No allocation churn inside the timing window — every benchmark
 *      pre-allocates its working set.
 *   4. Quick to run (< 30 seconds on the perf preset) so CI doesn't
 *      time out.
 *
 * Output format (one bench = one line on stdout):
 *
 *   PERF<TAB>module<TAB>name<TAB>bytes<TAB>iters<TAB>ns_per_op<TAB>mb_per_sec
 *
 * The leading PERF token plus tab separators make the rows trivially
 * grep-able and diff-friendly. */

#ifdef DATAKIT_TEST
int perfRegressionTest(int argc, char *argv[]);
#endif
