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

## verify — build HEAD plus named paths in isolation

Builds and tests exactly HEAD plus the paths you name, so unrelated
uncommitted work in the tree cannot make a change look good or bad:

```sh
uv run --project tools dktools verify NAME --tests REGEX [--serial REGEX] \
    [--compiler clang22|gcc15] [--preset dev|debug] [--target T]... \
    [--root DIR] [-j N] (PATH[=FILE]... | --staged | --all-changed)
```

- Each `PATH` takes its working-tree content (or its deletion, when it
  is gone from the working tree); `PATH=FILE` stages `FILE`'s content
  at `PATH`. The tree is written through a private `GIT_INDEX_FILE`
  (the real index is never touched), exported with `git archive` into
  `ROOT/verify/NAME/src`, then configured and built there. `ROOT` is
  `--root`, else `$DKTOOLS_SCRATCH`, else `dktools/` under the system
  temp dir.
- Instead of `PATH`s: `--staged` builds exactly the current index
  (HEAD plus what is staged, written from a copy of the index), so a
  64-file vendored re-land staged by `vendorsync --stage` needs no path
  list. `--all-changed` builds HEAD plus every tracked path that
  differs in the working tree: modified, added to the index, or
  deleted. Untracked files never take part. The two flags exclude each
  other and `PATH` arguments.
- Either flag refuses (exit 2) when the tree would be plain HEAD
  (nothing staged, or no tracked path changed): an empty change set
  there is a mistake, not a request. To build HEAD on purpose, name an
  unchanged `PATH`.
- The result's `tree` line says what was taken, and a `changed` line
  follows for every path that differs from HEAD (`A`, `M`, `D` or `T`
  and the path), in every mode.
- `--tests` runs in parallel (`-j N`, default the CPU count);
  `--serial` runs one test at a time (the timing-sensitive tests, as in
  `validate`'s default serial set) and is excluded from the parallel
  run. Both are
  whole-name ctest regexes, and one that matches nothing fails.
- Without `--target`, the build covers the four preset targets plus
  every target the selected tests run. Configure writes the CMake File
  API codemodel (each target's artifacts), `ctest --show-only=json-v1`
  names the selected tests, `src/dktools/ctestCommands.cmake`
  (`cmake -P`) reads every test's command line from the build's
  `CTestTestfile.cmake` files (ctest's JSON leaves out the command of
  a test whose executable is not built yet), and a test needs every
  target whose artifact its command names. `--tests 'varint-.*'`
  therefore builds `deps/varint`'s test executables too; the `targets`
  line lists what was added. `--target` builds exactly the targets
  given (`--target all` builds everything).
- A test whose executable was still not built (an explicit `--target`
  list that leaves it out) is reported on a `missing` line, apart from
  real failures, with a `hint` naming the targets to add.
- `allocaudit check --constructors` then runs on the exported tree
  with the tree's own dktools.
- Prints a result block (tree hash, build with its warning-line and
  linker-note counts, ctest, sanitizer reports from the test output,
  allocaudit, failing test names, log paths), also kept in
  `ROOT/verify/NAME/result.txt`. Exits 0 only when everything passed
  with 0 warning lines and no sanitizer report, 1 otherwise, 2 on an
  input error (one stderr line).
- `warning: no debug symbols in executable (-arch ARCH)` lines are
  macOS linker notes (`dsymutil` run on an executable built without
  debug info, as in an all-targets Release build), not compiler
  warnings: they show as `linker notes N` and never fail the run.
  Every other `warning:` line, `ld: warning:` included, still fails.
  `validate` counts them the same way.
- Every sanitizer report (UBSan `runtime error:`, ASan/LSan/TSan
  `ERROR:`) fails the run and prints as
  `report    TEST: FIRST LINE (LOG:LINE)`: the test whose section of
  ctest's output log holds it and the report's first line in full. The
  `sanitizer` line counts the reports and names the tests.

Compilers and presets live in `src/dktools/toolchain.py`, the single
place builds get them from; the system `cc` is never used:

| name                | programs                                                                                                         | presets                                             |
| ------------------- | ---------------------------------------------------------------------------------------------------------------- | --------------------------------------------------- |
| `clang22` (default) | `clang-22`, `clang++`, `llvm-ar`, `llvm-ranlib` from `--llvm-dir` / `$DKTOOLS_LLVM_DIR` (default: Homebrew LLVM) | `dev`, `debug` (CMakePresets.json)                  |
| `gcc15`             | `gcc-15`, `g++-15`, overridden by `--gcc-cc`/`--gcc-cxx` or `$DKTOOLS_GCC_CC`/`$DKTOOLS_GCC_CXX`                 | `release` (`-DCMAKE_BUILD_TYPE=Release`, no preset) |

## validate — the full matrix on a revision

Runs every compiler and preset on a committed revision, exported with
`git archive` so uncommitted work never takes part:

```sh
uv run --project tools dktools validate [--rev REV] \
    [--compilers clang22,gcc15] [--presets dev,debug] \
    [--serial REGEX] [--skip REGEX] [--debug-skip REGEX] \
    [--leak-base BASE] [--root DIR] [-j N]
```

- The matrix is every requested preset each compiler supports:
  by default `clang22-dev`, `clang22-debug` and `gcc15-release`.
- Per configuration: a clean configure, an all-targets build,
  warnings grouped by file, the parallel ctest set (everything outside
  the serial and skip sets), then the serial set one test at a time.
- Default serial set: `dkTestNonceLogEvictionPerf`, the one
  timing-sensitive test. It times 20000 evictions with the monotonic
  clock and fails above 5000 ns each, so it runs alone, where the
  parallel set cannot preempt it. Everything else runs in parallel:
  the persist tests (the `*P` wrappers, `persist`, `persistCtx`) each
  take a private `mkdtemp` scratch directory, and
  `dkTestKeyringGetRawCopyMigrationCluster` asserts its thread race
  deterministically.
  Default skip set: `varint-.*`, `perfRegression`, `compressionBench`.
  Debug builds also skip `multimap` and `multiFenwick` (too slow under
  Debug with sanitizers; `--debug-skip ''` runs them), and the summary
  says so.
- Then: UBSan (`runtime error:`) and ASan/LSan reports in the test
  output, `allocaudit check --constructors` on the exported tree, and
  the leakage gate over `merge-base(BASE, REV)..REV` (`BASE` defaults
  to `origin/main`; `--leak-base ''` skips it).
- Writes `ROOT/validate/<hash>/summary.md` (warnings split into
  datakit `src/`, `deps/` and other) and prints it. Exits 0 only when
  everything passed with no warning, sanitizer report or hard leak
  hit; 1 on any failure; 2 on an input error.

## leakcheck — the leakage gate

Checks text about to enter history for terms that must never appear in
datakit (downstream project names and internals, project-management
labels, scratch timestamps, local filesystem paths):

```sh
uv run --project tools dktools leakcheck                # staged diff (default)
uv run --project tools dktools leakcheck --range A..B   # added lines of A..B
uv run --project tools dktools leakcheck --tree         # every tracked file
```

- `--staged` and `--range` check only added lines (`A...B` diffs
  from the merge base); `--tree` checks every line of every tracked
  file under `src/`, `docs/`, `include/`, `tools/`, and the top-level
  `*.md` files.
- Vendored code (`deps/`, `src/memshard/`) mirrors upstream and is
  skipped unless `--include-vendored` is given.
- Each hit prints as `path:line: [term] text`. Hard terms fail the
  gate (exit 1); patterns that also match ordinary code print under a
  separate `REVIEW` heading and do not change the exit code. Exits 0
  with no hard hit, 2 on an input error.
- The term list and its matching rules live in
  `src/dktools/leakcheck.py`, the single source of truth. Multi-word
  terms match with a space, hyphen, underscore or nothing between the
  words.

## vendorsync — mirror a vendored dependency

Makes a vendored tree hold exactly its upstream's tracked files at a
revision (supported: `varint`, at `deps/varint`):

```sh
uv run --project tools dktools vendorsync varint --upstream DIR [--rev REV] \
    [--stage] [--accept-local-loss] [--force-ignored] [--dry-run]
```

- Adds new files, rewrites changed ones (content and executable bit),
  and deletes files datakit tracks there that upstream no longer has.
  Upstream content comes from `git ls-tree` and `git cat-file`, so
  upstream export attributes cannot alter it. A second run at the same
  revision writes nothing.
- Before writing, every datakit version the sync replaces (HEAD,
  index and working tree) is looked up in upstream's whole history.
  One upstream never had is a datakit-local edit: it is printed with
  its diff against the upstream revision, and the sync refuses
  (exit 1) unless `--accept-local-loss` is given.
- Upstream files that datakit's `.gitignore` hides are written but
  listed separately, since `git add` would skip them silently;
  `--stage --force-ignored` stages them with `git add -f`.
- `--stage` stages exactly the paths whose index entry differs from
  upstream, by explicit path. Without it the files are written and
  listed; `--dry-run` writes and stages nothing.
- Prints the upstream revision and subject, then added, modified and
  deleted counts for the working tree (this run's writes) and against
  the index (what `--stage` stages), the ignored files, and any
  untracked extras (not upstream, left alone). Exits 0 on success, 1
  on a refusal, 2 on an input error.

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
