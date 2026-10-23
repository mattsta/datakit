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
  per-file with a process pool (`-j N`, default the CPU count); compile
  args come from `build/compile_commands.json`, headers parse as plain
  C. The whole tree takes a few seconds; `--timings N` prints the N
  slowest files to stderr.
- The pool starts its workers with `spawn` (each loads libclang
  itself) and hands out the largest files first. It never hangs: a
  worker that dies (a signal, out of memory) or a run where no file
  finishes for `--timeout` seconds (default 120, `0` waits forever)
  exits 2 with one stderr line naming the files involved, after every
  worker is killed. Workers ignore Ctrl-C (dktools stops them) and
  exit by themselves if dktools is killed.

## atomicaudit — atomic memory-order audit

Every atomic operation in `src/` (vendored `memshard` excepted) carries
a `@sync` tag saying why its memory order is what it is, on or
directly above the operation:

```c
/* @sync: release arena.retire */          /* publishes along an edge */
/* @sync: acquire arena.retire */          /* observes it */
/* @sync: seq_cst gate.count */            /* needs the total order */
/* @sync: relaxed counter */               /* counter|seed|owner|init|hint */
/* @sync: local timing */                  /* timing|signal: not for threads */
```

`check` verifies each operation's order provides exactly what its tag
claims (UNDER-ORDERED and OVER-ORDERED are both violations), that the
order is valid for the operation, that every named edge has both a
publishing and an observing side, and that a seq_cst edge is seq_cst
everywhere.  `report` prints the edge map: who publishes and who
observes each edge.  The grammar and rules are in
`src/dktools/atomicaudit.py`.

```sh
uv run dktools atomicaudit check                # whole tree (the gate)
uv run dktools atomicaudit check src/a.c src/a.h  # a subset while working
uv run dktools atomicaudit report               # markdown edge map
```

It is enforced as the ctest test `atomicOrdering` (registered beside
`allocOwnership`), and `verify` and `validate` run it on the tree they
build (the `atomic` line).  Clang builds add `-Watomic-implicit-seq-cst`,
which catches the operator forms (`x++` on an `_Atomic`) a lexical audit
cannot see.

The allocation audit is enforced as the ctest test `allocOwnership`
(registered in `src/CMakeLists.txt` whenever `uv` is on PATH), so it
runs with plain `ctest`, `datakit-check`, and `datakit-quickcheck`.
The tools' own tests (`tests/`, stdlib `unittest`) run as the ctest
test `dktoolsSelfTest`, or directly from the repository root:

```sh
uv run --project tools python -m unittest discover -s tools/tests
```

Both ctest entries go through `uvGate.sh`, which reports the test as
skipped rather than failed when `uv sync` cannot set up this
environment (an offline machine with nothing cached), and runs uv with
`--frozen` so a test never rewrites `uv.lock`. Only that setup step
can skip: once the environment exists, the command's own status,
a crash or a signal included, is the gate's status.

Coding-practice documentation: `docs/development/CONTRIBUTING.md`
(Memory Management) and the idiom block in `src/cleaner.h`.

## Child processes

Every program dktools runs (git, cmake, the build, ctest, the
allocaudit gate) goes through `src/dktools/procs.py`:

- An argument list, never a shell; stdin is `/dev/null`, so nothing
  can wait on a prompt.
- Each program runs in its own session. On a timeout, Ctrl-C, SIGTERM
  or SIGHUP, its whole process group (cmake, make, the compilers,
  ctest and the tests it started) gets SIGTERM, then SIGKILL two
  seconds later, and is reaped before dktools exits: Ctrl-C (exit 130) leaves nothing running. Git gets the same two seconds to
  remove its lock files.
- Output goes to a log file or is read with `communicate`, so no
  pipe can fill and stall a program.
- Limits: 10 minutes per git command (an input error, exit 2); in
  `verify` and `validate`, 30 minutes for configure and allocaudit,
  10 for the test listing, 3 hours for a build or a ctest run, and
  `ctest --timeout 1500` for each test without its own TIMEOUT. A
  step past its limit is killed, its log ends with a `timed out`
  line, and it fails the run (`FAILED (timed out)`).
- `kill -USR1 PID` makes a dktools process (or a pool worker) print
  every thread's stack to stderr, to see where something waits.
- An interrupted `verify` or `validate` leaves a partial work dir
  under `ROOT`; the next run with the same name or revision recreates
  it. The private index `verify` writes the tree through lives in a
  temporary directory that is always removed.

## verify — build HEAD plus one change in isolation

Builds and tests exactly HEAD plus the paths you name (or the index,
or every changed tracked path), so unrelated uncommitted work in the
tree cannot make a change look good or bad:

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
  (`index matches HEAD`, `no tracked changes`): an empty change set
  there is a mistake, not a request. To build HEAD on purpose, name an
  unchanged `PATH`. A refused run, like any other input error found
  while building the tree, leaves the previous result under `NAME`
  untouched.
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
    [--tsan-skip REGEX] [--full] [--slow REGEX] [--debug-slow REGEX] \
    [--leak-base BASE] [--root DIR] [-j N]
```

- The matrix is every requested preset each compiler supports:
  by default `clang22-dev`, `clang22-debug` and `gcc15-release`;
  `--presets dev,debug,tsan` adds `clang22-tsan`.
- Two tiers. The default quick tier is for iteration: it skips the
  slow tests, `--slow` in every build (over ~40 s under dev) and also
  `--debug-slow` in debug and tsan builds (over ~60 s there).
  `--full` is the milestone tier and runs them all. Both sets are
  empty by default: the suites that used to be in them run alone in
  about 10 s or less under dev. A suite that must stay long belongs
  in a set, not in every quick run. `verify` has no tiers; it runs exactly the tests it is
  given.
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
  A serial set whose tests are all skipped in a build is dropped there
  rather than reported as "no tests".
  Default skip set: `varint-.*`, `perfRegression`, `compressionBench`,
  `fenwickBench` (vendored suites and benchmarks).
  Tests a build cannot judge are skipped in both tiers: none in debug
  (`--debug-skip`), `dkTestNonceLogEvictionPerf` in tsan
  (`--tsan-skip`; its wall-clock limit cannot hold under TSan). The
  summary's "Skipped tests" section names the tier and every skipped
  set per configuration.
- Then: UBSan (`runtime error:`) and ASan/LSan reports in the test
  output, `allocaudit check --constructors` on the exported tree, and
  the leakage gate over `merge-base(BASE, REV)..REV` (`BASE` defaults
  to `origin/main`; `--leak-base ''` skips it).
- Writes `ROOT/validate/<hash>-<configs>/summary.md` (`<configs>` joins
  the matrix's labels, e.g. `clang22-dev+clang22-debug+gcc15-release`, so a
  TSan matrix on the same revision keeps the default one's logs; warnings split into
  datakit `src/`, `deps/` and other) and prints it. Exits 0 only when
  everything passed with no warning, sanitizer report or hard leak
  hit; 1 on any failure; 2 on an input error.

## coverage — what the tests reach, and the public API they never call

```sh
uv run --project tools dktools coverage [--rev REV] [--top N] [--skip REGEX] \
    [--serial REGEX] [--root DIR] [-j N] [--reuse]
```

- Exports REV (default HEAD) like validate, builds the `coverage`
  preset (clang 22, Release, `-fprofile-instr-generate
  -fcoverage-mapping`, no sanitizers), runs every test with its own
  profile file (the serial set alone, as validate does), and merges
  the profiles with `llvm-profdata`.
- Reports, over the library sources (`deps/`, `*Test.c`, `*Bench.c`
  and `datakit-test.c` left out; a module's in-file tests count with
  it): line, function, branch and region totals, and the N
  least-covered files.
- Splits the public API (every function declared in a `src/` header
  other than `*Internal.h` / `*Impl.h`, without the `*Test` entry
  points) into called, never called (count 0), and not built (no
  counters in the test binary: a declaration without a definition, or
  a platform's).
- Lists the functions any `src/` header defines inline (`static inline`,
  `DK_INLINE_ALWAYS`) that no compiled copy ran.  Such a function gets
  one copy per including file, so a header's own line numbers also count
  the unused copies of functions that do run; summed by name over the
  copies, this list holds only the ones that never ran anywhere.
- `--reuse` renders the summary again from the earlier run for REV (no
  build, no tests), for changes to the report itself.
- A module's in-file tests (`#ifdef DATAKIT_TEST` blocks) count in the
  llvm-cov totals, and their failure branches never run in a passing
  suite.  The "Library code" totals and the per-file tables leave them
  out (per-line counts from `llvm-cov export -format=lcov`; a header's
  inline function counts as run if any copy ran).
- The coverage ratchet: `tools/coverage-baseline.json` records each
  file's uncovered library lines and branches.  A run fails (exit 1)
  when any file has more of either than its baseline, lists the files
  that improved and the new ones, and `--update-baseline` writes the
  run's figures to the working tree (commit it with the tests that
  earned it).  A run that regressed is not written unless
  `--accept-regressions` says the drop is intended.  When the baseline commit's run is still under
  `ROOT/coverage/`, a regression also lists each newly uncovered line
  and branch with its source (matched by source text, so line shifts
  from edits don't count); otherwise re-run coverage at that commit.
  The coverage preset updates counters atomically: plain counters lose
  counts in threaded tests, and the branch counts llvm derives from them
  then drift by one or wrap, which read as coverage changes.
- Volatile points: coverage that depends on thread scheduling (a CAS
  that fails only under contention, a child process that exits first)
  differs between runs of the same sources.  The baseline lists such
  points (kind, source line and the line before it) and every count
  leaves them out.  `--learn-volatile` finds them by diffing two runs of
  the same sources: a second run at the same commit keeps the first as
  `ROOT/coverage/<hash>.prev` and diffs against it; otherwise it diffs
  against the baseline commit's cached run, if `src/` and the CMake files
  match.  Every point that differs is added and the baseline rewritten.
  It refuses a failed run, changed sources, or more than 100 differing
  points (not noise).  A point that is lucky in the baseline run but
  missed in both reruns shows as a regression: review it and record it
  with `--accept-regressions`; a later rerun that flips it learns it.
- `--detail FILE` (repeatable, repo-relative) reads the earlier run for
  REV and lists FILE's functions by missed lines and its uncovered line
  ranges (each with its first source line): the worklist for one file.
- Writes `ROOT/coverage/<hash>/summary.md` and `summary.json` and
  prints the summary. There is no percentage target: the output is
  the list of gaps to triage. Exits 0 when every test passed, 1
  otherwise (the report is still written once the build succeeded), 2
  on an input error.

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
- `QUOTED_OUTPUT` lets a named file contain a named term because it
  quotes another program's fixed output (dktools recognises dsymutil's
  note naming the linker's temporary LTO object); every other term
  still applies there.
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

## testtime — where a slow test spends its time

Runs one datakit test and times each section it prints:

```sh
uv run --project tools dktools testtime ./build-dev/src/datakit-test multimap [--top N] [--timeout SECONDS] [--prefix P]
```

- Sections open at ctest.h's `test — NAME` headers and at the headers
  suites print themselves (`Test `, `Testing `, `Stress `, `FUZZ: `);
  `--prefix` adds more (repeatable). datakit-test line-buffers stdout,
  so every header arrives when printed, even through the pipe.
- A section lasts from its header to the next one (the last to the
  program's exit); the report lists the total, the N slowest section
  groups (names with their numbers masked, so generated sections add
  up) and the N slowest sections (default 15), with their share.
- The test runs in its own process group and is stopped, children
  included, after `--timeout` seconds (default 1800).
- Exits 0 when the test passes, 1 when it fails or is stopped, 2 on an
  input error (such as a missing binary).

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

## perfcompare — a change's performance against a base revision

Builds a base revision and the working tree side by side and times them
on the same perfRegression benchmarks:

```sh
uv run dktools perfcompare NAME --base REV [--modules M[,M...]] \
    [--runs N] [--tolerance PCT] [--bench-path PATH ...] \
    [--compiler clang22|gcc15] [--root DIR] [-j N]
```

- `current` is HEAD plus every changed tracked path (as `verify
  --all-changed`); `base` is REV with the benchmark sources
  (`--bench-path`, default `src/perfRegression.c` and `.h`) taken from
  the working tree, so a benchmark added for the change also times the
  base.  Both build only `datakit-test`, with the perf preset (gcc15:
  release), under `ROOT/perfcompare/NAME/{base,current}`.
- `datakit-test test perfRegression` runs on the two builds alternately,
  `--runs` times each (default 5; the first side alternates by round),
  limited to `--modules` through `DK_PERF_MODULES`.  Each run is kept as
  `run-<side>-<round>.tsv`.
- Prints, and writes to `result.txt`, the median ns/op of each side per
  row (module, name, bytes) with the change in percent, and the rows only
  one side has.
- Exits 0; 1 when `--tolerance PCT` is given and a row's median grew by
  more than PCT percent, or when a build or run fails; 2 on an input
  error.

Use it for every before/after claim about a change: the benchmark lands
in perfRegression with the change, and the comparison is reproducible.
