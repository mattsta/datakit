# dktools — datakit platform automation

The home for all long-lived platform automation. New helpers become
`dktools` subcommands here rather than ad-hoc scripts scattered around
the tree.

A [uv](https://docs.astral.sh/uv/) project; everything runs through it:

```sh
cd tools
uv run dktools --help
```

## allocaudit — allocation-ownership audit

Every heap allocation and container-constructor call in `src/` must be
either SCOPED (a `cleaner.h` scope-exit attribute frees it on every
exit path) or ANNOTATED with the machine-validated grammar
`@own: (caller|container|static|cache)(; free=IDENT)?` on or directly
above the allocation line. The full contract, classification
heuristics, and the `CONSTRUCTORS` registry (constructor → releasing
function — the single place new containers get registered) live in
`src/dktools/allocaudit.py`.

```sh
uv run dktools allocaudit check                 # allocators only
uv run dktools allocaudit check --constructors  # full discipline (the gate)
uv run dktools allocaudit report --constructors # markdown report
```

- `check` exits 0 only on `100% clean pattern`; any violation, parse
  error, or malformed `@own` annotation is nonzero.
- `report` writes a markdown report to stdout (or to `--out PATH`)
  with per-site data-flow classification and leak-risk ranking to
  guide migration work; the summary line goes to stderr when the
  report goes to stdout.
- Discovery is lexical over libclang's pre-expansion token stream
  (macro-definition sites are audited at the definition), parallelized
  per-file with a process pool (`-j N`); compile args come from
  `build/compile_commands.json`, headers parse as plain C.

The audit is enforced as the ctest test `allocOwnership`
(registered in `src/CMakeLists.txt` whenever `uv` is on PATH), so it
runs with plain `ctest`, `datakit-check`, and `datakit-quickcheck`.
The tools' own tests (`tests/`, stdlib `unittest`) run as the ctest
test `dktoolsSelfTest`, or directly from the repository root:

```sh
uv run --project tools python -m unittest discover -s tools/tests
```

Both ctest entries go through `uvGate.sh`, which reports the test as
skipped rather than failed when uv cannot set up this environment (an
offline machine with nothing cached), and runs uv with `--frozen` so a
test never rewrites `uv.lock`.

Coding-practice documentation: `docs/development/CONTRIBUTING.md`
(Memory Management) and the idiom block in `src/cleaner.h`.

## perfdiff — perf regression comparison

Compares two `perfRegression` TSV outputs row by row, keyed on
(module, name, bytes):

```sh
uv run dktools perfdiff BASELINE.tsv CURRENT.tsv [--tolerance PCT]
```

- Exits 0 when every matched row is within the tolerance (default
  10%), 1 when any row's ns_per_op grew or mb_per_sec fell by more
  than the tolerance (the offending rows print to stdout as TSV under
  a `REGRESSION` header), and 2 on any input error: a file that is
  missing, unreadable, a directory, or not UTF-8, a malformed row, a
  metric that is not a finite non-negative number, or a bad
  `--tolerance`. Input errors print one line to stderr, never a
  traceback.
- A baseline metric of 0 (printed for sub-precision values and for
  mb_per_sec of zero-byte rows) is not compared; a row whose baseline
  metrics are both 0 is listed on stderr as skipped.
- Rows missing from CURRENT or new in it are listed on stderr and do
  not change the exit code.
- The baseline is a local file, not committed; generate it from a
  perf build with
  `./build-perf/src/datakit-test test perfRegression > perf.baseline.tsv`.
  Paths are relative to `tools/` when run as shown; from the repository
  root, where the TSVs are generated, use
  `uv run --project tools dktools perfdiff perf.baseline.tsv perf.current.tsv`.
