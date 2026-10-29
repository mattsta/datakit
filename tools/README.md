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

A call to a libc allocator (`malloc`, `calloc`, `realloc`, `free`,
`posix_memalign`, `aligned_alloc`, `strdup`, `strndup`) is a violation:
datakit allocates through `zmalloc`/`zcalloc`/`zrealloc`/`zfree`. A file
opts out only explicitly, with `#define IKNOWWHATIMDOING` (the allocator
implementation and its configuration, a libc baseline benchmark, a
standalone program).

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
everywhere. `report` prints the edge map: who publishes and who
observes each edge. The grammar and rules are in
`src/dktools/atomicaudit.py`.

```sh
uv run dktools atomicaudit check                # whole tree (the gate)
uv run dktools atomicaudit check src/a.c src/a.h  # a subset while working
uv run dktools atomicaudit report               # markdown edge map
```

It is enforced as the ctest test `atomicOrdering` (registered beside
`allocOwnership`), and `verify` and `validate` run it on the tree they
build (the `atomic` line). Clang builds add `-Watomic-implicit-seq-cst`,
which catches the operator forms (`x++` on an `_Atomic`) a lexical audit
cannot see.

## outputaudit — library writes to stdout/stderr

datakit runs inside other applications, which own the process's standard
streams. `outputaudit` lists every call in `src/` that writes to stdout
or stderr (`printf`, `fprintf(stderr, ...)`, `write(2, ...)`, ...),
outside in-file test code, and sorts each: `unrequested` (printed without
the caller asking), `debug` (only under a DEBUG/VERBOSE/NOISY/TRACE/DBG/
DUMP switch or `#ifndef NDEBUG`), `dead` (`#if 0`), `requested` (inside a
Repr/Print/Dump/Say/Show function), `tool` (the operator tools dataspeed
and memtest, whose output is their job) and `test` (test-only files).
Writes to a stream the caller passed in are the caller's output and not
listed. Library code reports misuse and conditions through dkDiag
(`src/dkDiag.h`) instead of printing.

```sh
uv run dktools outputaudit report    # markdown, by category, file and function
uv run dktools outputaudit check     # exit 1 while unrequested output remains
```

It is enforced as the ctest test `libraryOutput` (beside
`atomicOrdering`), and `verify` and `validate` run it on the tree they
build (the `output` line).

## fusionscan — two searches where one could do

`fusionscan` lists, for review, every function in library code that reads
a container by a key (a call whose name after its family is Find, Exists,
Lookup, Get, Contains, Has, Search or Peek) and then writes the same
container by the same key (Insert, Add, Delete, Remove, Replace, Set,
Incr, Update, Push, Append, Resize, Del, Put). Families are the header
names under `src/`, longest first, so `multiOrderedSetExists` is family
`multiOrderedSet`; containers and keys compare by root identifier. A read
whose result a write takes (the write acts where the read landed) is not
paired. Test-only files, in-file test code, `#if 0` and macro bodies are
skipped. It is a review list, not a gate: some pairs are needed (the read
decides what the write cannot).

```sh
uv run dktools fusionscan                  # markdown table to stdout
uv run dktools fusionscan --out report.md  # summary line, report to a file
```

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

### Machine share

Every command that builds or tests (`verify`, `validate`, `coverage`,
`perfcompare`, `tune`, the allocaudit pool) takes only part of the
machine, so a run leaves it usable:

- dktools runs `nice` 10, and every build, test and audit it starts
  inherits that.
- Compile jobs: `-j N`, else `DKTOOLS_JOBS`, else half the performance
  cores (4 on an 8+2-core Apple M-series).
- Tests at once: `DKTOOLS_TEST_JOBS`, else half the compile jobs; many
  tests start threads of their own.
- Load ceiling: make (`-l`) and ctest (`--test-load`) start no new job
  while the load average is at or above `DKTOOLS_LOAD`, else the
  performance-core count. `DKTOOLS_LOAD=0` removes it: on a machine
  another workload keeps above the ceiling, ctest would otherwise wait
  without starting a test.
- Run one heavy command at a time: each one already sizes itself to
  the machine.

## format — every formatter, one command

```sh
uv run --project tools dktools format write            # files changed from HEAD + untracked
uv run --project tools dktools format check --all      # every file; rewrites nothing
uv run --project tools dktools format write PATH...    # just these
```

| files      | formatter                                                                                                                                       |
| ---------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| `.c`, `.h` | clang-format with the repository's `.clang-format`, from the clang 22 bin directory (`$DKTOOLS_LLVM_DIR`)                                       |
| `.md`      | prettier (reads `.prettierignore`)                                                                                                              |
| `.py`      | `ruff check --select SIM,PTH,I,UP,FURB,PGH --extend-select F401 --ignore SIM102,SIM110 --fix`, then `ruff format`; the ruff locked in `uv.lock` |

- `deps/` is vendored and never touched. Without `PATH`s or `--all`,
  the files are those changed from HEAD plus untracked ones git does
  not ignore.
- `write` rewrites in place and names every file it changed; `check`
  changes nothing and names every file `write` would change. A lint
  finding ruff cannot fix, or a formatter that fails on a file, is a
  problem in both modes and fails the run (exit 1).
- `verify` formats the working-tree files it takes before building,
  and both `verify` and `validate` check every file of the tree they
  build (the `format` line / `## Formatting` section). Never run the
  formatters by hand: this command is the one place their flags live
  (`src/dktools/formatting.py`).
- Every program dktools runs is either a locked dependency of this
  project (`ruff`, `git-filter-repo`) or part of the toolchain
  (clang-format beside clang 22). prettier is the exception: it is
  not a Python package and comes from `PATH`.

## historyscrub — remove text from every version of files in history

```sh
uv run --project tools dktools historyscrub check   --from-commit REV [--forbid REGEX]...
uv run --project tools dktools historyscrub rewrite --from-commit REV [--forbid REGEX]... [--name N]
uv run --project tools dktools historyscrub map OLDHASH... [--name N]
```

- The edits are the hunks of commit `REV`: each hunk's old lines
  become its new lines, in every version of that file in every ref.
  `REV` then changes nothing and the rewrite drops it, so history
  reads as if the text was never there. Only modified files, and
  hunks that remove or replace lines, qualify.
- `check` changes nothing. It reports each file's versions in history
  and how many the edits change, any version still matching a
  `--forbid` pattern after the edits (a variant the edits miss), and
  any other path holding a hunk's old text. It exits 1 unless clean.
- `rewrite` refuses unless `check` is clean and no tracked file has
  changes. It then:
  - writes a bundle of every ref to
    `build/dktools/historyscrub/NAME/backup.bundle` (the one copy of
    the old history; collected with the other builds after 14 days);
  - rewrites every ref with git filter-repo, which also rewrites old
    hashes quoted in commit messages;
  - verifies that the check is clean, every ref still exists, only
    the commits the edits emptied were dropped, and HEAD's tree is
    unchanged when HEAD already held the edits;
  - expires reflogs and prunes, so the old objects leave the
    repository.

  Remotes stay configured, and nothing is pushed.

- `map` translates old commit hashes (unique prefixes) to new ones
  through the run's commit map, for hashes recorded outside commit
  messages.

## builds — the build inventory and its garbage collection

Every build lives under `build/`: CMake presets in `build/<preset>`,
dktools work directories in `build/dktools/<kind>/<name>` (the layout
table is in [BUILD_PRESETS.md](../docs/BUILD_PRESETS.md)).

```sh
uv run --project tools dktools builds list               # every build: kind, size, last use, state
uv run --project tools dktools builds gc --dry-run       # what a collection would remove
uv run --project tools dktools builds gc [--budget-gb N] [--max-age-days D] [--legacy] [--presets]
```

- Each dktools work directory holds a manifest (`.dkbuild.json`: kind,
  name, revision, command, created, last used, size) and, while a run
  uses it, a lock (`.dkbuild.lock`: the run's process id). A lock
  whose process is gone is idle. `coverage --reuse` counts as a use.
- `gc` removes idle dktools builds unused for `--max-age-days`
  (`$DKTOOLS_BUILD_MAX_AGE_DAYS`, default 14), then the least recently
  used until the rest fit `--budget-gb` (`$DKTOOLS_BUILD_BUDGET_GB`,
  default 40). A build in use is never removed, and preset builds only
  with `--presets`. `--legacy` also removes the builds made before this
  layout: root `build-*` directories, an old single build's top-level
  files in `build/`, and the old scratch root in the system temp
  directory.
- Every `verify`, `validate`, `coverage` and `perfcompare` run collects
  first (budget and age only; never legacy or presets) and prints what
  it removed.

## verify — build HEAD plus one change in isolation

Builds and tests exactly HEAD plus the paths you name (or the index,
or every changed tracked path), so unrelated uncommitted work in the
tree cannot make a change look good or bad:

```sh
uv run --project tools dktools verify NAME --tests REGEX [--serial REGEX] \
    [--compiler clang22|gcc15] [--preset dev|debug] [--also-build gcc15|clang22]... \
    [--target T]... [--define NAME=VALUE]... \
    [--root DIR] [-j N] (PATH[=FILE]... | --staged | --all-changed)
```

Iterative work: `--compiler clang22 --also-build gcc15` tests on clang 22
and only builds on gcc 15. The two-compiler test run is `validate`, the
gate before a milestone.

- Each `PATH` takes its working-tree content (or its deletion, when it
  is gone from the working tree); `PATH=FILE` stages `FILE`'s content
  at `PATH`. The tree is written through a private `GIT_INDEX_FILE`
  (the real index is never touched), exported with `git archive` into
  `ROOT/verify/NAME/src`, then configured and built there. `ROOT` is
  `--root`, else `$DKTOOLS_SCRATCH`, else `build/dktools` in the
  repository (see `builds` above).
- Before the tree is written, the working-tree files it takes are
  formatted in place (`format write` on the named paths, or every
  changed path with `--all-changed`; never with `--staged`, whose
  index is what is built). A `formatted` line names each file
  rewritten, and the `format` line checks every file of the built
  tree: an unformatted file fails the run.
- Instead of `PATH`s: `--staged` builds exactly the current index
  (HEAD plus what is staged, written from a copy of the index), so a
  64-file vendored re-land staged by `vendorsync --stage` needs no path
  list. `--all-changed` builds HEAD plus every tracked path that
  differs in the working tree (modified, added to the index, or
  deleted) and every untracked, unignored file under `src/` (a new
  header or source); untracked files elsewhere never take part. The
  two flags exclude each other and `PATH` arguments.
- Either flag refuses (exit 2) when the tree would be plain HEAD
  (`index matches HEAD`, `no changes`): an empty change set
  there is a mistake, not a request. To build HEAD on purpose, name an
  unchanged `PATH`. A refused run, like any other input error found
  while building the tree, leaves the previous result under `NAME`
  untouched.
- The result's `tree` line says what was taken, and a `changed` line
  follows for every path that differs from HEAD (`A`, `M`, `D` or `T`
  and the path), in every mode.
- `--also-build COMPILER` (repeatable) configures and builds the same
  tree and targets with another compiler at its default preset, after
  the main build, without tests; an `also` line reports it and its
  build and warnings join the gate. A `time` line gives each step's
  wall time and the total.
- With `--target` and no `--tests` / `--serial` the run is build-only
  (every other gate still runs). That is how the library is checked the
  way consumers compile it, with asserts off:
  `--preset perf --define DatakitTestMode=OFF --target datakit-static`
  (`DatakitTestMode` keeps `-UNDEBUG` otherwise).
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
- `--define NAME=VALUE` (repeatable) adds a cmake cache entry after the
  preset's, such as `--define UseFloat128C23=ON`; the config label
  names it, so the build directory differs from the plain one.
- A test whose executable was still not built (an explicit `--target`
  list that leaves it out) is reported on a `missing` line, apart from
  real failures, with a `hint` naming the targets to add.
- `allocaudit check --constructors`, `atomicaudit check`,
  `outputaudit check`, `spell`, `idioms` and `testchecks` then run on
  the exported tree with the tree's own dktools (the `alloc`, `atomic`,
  `output`, `spell`, `idioms` and `checks` lines).
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
    [--library-only] [--python] [--leak-base BASE] [--root DIR] [-j N]
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
  output, `allocaudit check --constructors`, `atomicaudit check`,
  `outputaudit check`, `spell`, `idioms` and `testchecks` on the
  exported tree, and the leakage
  gate over `merge-base(BASE, REV)..REV` (`BASE` defaults
  to `origin/main`; `--leak-base ''` skips it).
- `--python` also builds the Python package's wheel from the exported
  tree (the first configuration's compiler) and runs its tests on each
  interpreter, as `pybuild` / `pytest` do; the summary's "Python
  package" section reports the build and each interpreter.
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
  `DK_INLINE_ALWAYS`) that no compiled copy ran. Such a function gets
  one copy per including file, so a header's own line numbers also count
  the unused copies of functions that do run; summed by name over the
  copies, this list holds only the ones that never ran anywhere.
- `--reuse` renders the summary again from the earlier run for REV (no
  build, no tests), for changes to the report itself.
- A module's in-file tests (`#ifdef DATAKIT_TEST` blocks) count in the
  llvm-cov totals, and their failure branches never run in a passing
  suite. The "Library code" totals and the per-file tables leave them
  out (per-line counts from `llvm-cov export -format=lcov`; a header's
  inline function counts as run if any copy ran).
- The coverage ratchet: `tools/coverage-baseline.json` records each
  file's uncovered library lines and branches. A run fails (exit 1)
  when any file has more of either than its baseline, lists the files
  that improved and the new ones, and `--update-baseline` writes the
  run's figures to the working tree (commit it with the tests that
  earned it). A run that regressed is not written unless
  `--accept-regressions` says the drop is intended. When the baseline commit's run is still under
  `ROOT/coverage/`, a regression also lists each newly uncovered line
  and branch with its source (matched by source text, so line shifts
  from edits don't count); otherwise re-run coverage at that commit.
  The coverage preset updates counters atomically: plain counters lose
  counts in threaded tests, and the branch counts llvm derives from them
  then drift by one or wrap, which read as coverage changes.
- Volatile points: coverage that depends on thread scheduling (a CAS
  that fails only under contention, a child process that exits first)
  differs between runs of the same sources. The baseline lists such
  points (kind, source line and the line before it) and every count
  leaves them out. `--learn-volatile` finds them by diffing two runs of
  the same sources: a second run at the same commit keeps the first as
  `ROOT/coverage/<hash>.prev` and diffs against it; otherwise it diffs
  against the baseline commit's cached run, if `src/` and the CMake files
  match. Every point that differs is added and the baseline rewritten.
  It refuses a failed run, changed sources, or more than 100 differing
  points (not noise). A point that is lucky in the baseline run but
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

## commit — commit exactly what a passing verify built

```sh
uv run --project tools dktools commit NAME -F MESSAGE PATH... [--root DIR]
```

- Refuses (exit 1) unless `verify NAME` passed and each `PATH`'s
  working-tree content, or its deletion, is byte-for-byte what that
  verify exported and built. A file edited after the verify cannot slip
  into the commit.
- Refuses while the index holds anything staged, so the commit is
  exactly the named `PATH`s; stages them (added, changed or deleted).
- Runs the leakage gate on the staged diff; a hard hit unstages these
  `PATH`s and commits nothing.
- `MESSAGE` must carry the `What:`, `Why:` and `How:` paragraphs
  (exit 2 otherwise).
- The cycle is `verify NAME --compiler clang22 --also-build gcc15
--tests REGEX PATH...`, then `commit NAME -F MESSAGE PATH...`.

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
uv run --project tools dktools testtime ./build/dev/src/datakit-test multimap [--top N] [--timeout SECONDS] [--prefix P]
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

## configure — refresh a preset build's compile database

```sh
uv run --project tools dktools configure [dev] [--compiler clang22]
```

- Runs `cmake --preset PRESET` in the repository with the named
  compiler, so `build/PRESET` and its `compile_commands.json` (the
  database clangd reads, see `.clangd.sample`) follow the current CMake
  files. Run it after a change of flags, standard or compiler.
- Configures only. Exits 0 when cmake configured, 1 when it failed (its
  output is printed), 2 for a preset the compiler does not build.

## wrapcalls — wrap the calls that drop a `[[nodiscard]]` result

After marking a type `[[nodiscard]]`, a build lists every call that
drops one. This wraps each reported call in a macro the test or file
defines (`call(...);` becomes `MACRO(call(...));`):

```sh
uv run --project tools dktools wrapcalls build/dktools/verify/NAME/build.log --type clusterResult --macro CR_OK [--path src/clusterRing.c] [--dry-run]
```

- Reads clang's "ignoring return value of type 'T' declared with
  'nodiscard'" warnings; a path inside a verify or validate scratch
  tree maps back to the repository.
- Wraps a site only when the call is a whole statement: only
  whitespace before it on its line, the previous code character one
  of `; { } ) :`, and `;` after its closing parenthesis. Comments,
  literals and preprocessor lines are skipped while matching
  parentheses. Every other site is listed as skipped, for a hand edit.
- The log's line numbers must match the files: wrap before adding
  lines (such as the macro's definition) to a file it names.
- `--path` (repeatable) limits it to the named files.
- Exits 0 when every site was wrapped, 1 when any was skipped.

## retoken — rename a C token where code uses it

```sh
uv run --project tools dktools retoken _Static_assert static_assert [--comments] [--dry-run] [FILE ...]
```

- Renames whole-identifier occurrences in the tracked `src/**/*.c` and
  `*.h` (vendored `src/memshard` excluded), or in the files given.
- Macro bodies are code and change; string and character literals
  never do; comments change only with `--comments` (prose naming the
  token).
- `--text` replaces an exact code text instead of a token
  (`retoken --text '= {0}' '= {}'`), only where every byte of it is
  code.
- Prints each file's count. Exits 0, or 2 for a token that is not a C
  identifier or a missing file.

## pybuild / pytest — the Python package's wheel and its tests

```sh
uv run --project tools dktools pybuild NAME [--compiler clang22]
uv run --project tools dktools pytest NAME [--python 3.15,3.15t] [--tests PATTERN]
```

- `pybuild` builds `python/` (the `datakit` package over its C
  extension) into `build/dktools/pybuild/NAME/wheel`: one wheel tagged
  `cp315-abi3.abi3t`, which loads on the regular and the free-threaded
  interpreter. The free-threaded 3.15 builds it (the build backend makes
  the shared `abi3t` binary only there). The backend's versions are
  pinned, with hashes, in `python/build-constraints.txt` (compiled from
  `python/build-requirements.in` by `uv pip compile`); uv itself is a
  locked tools dependency.
- `pytest` installs that wheel into a fresh environment per interpreter
  (uv-managed interpreters only) and runs `python/tests` (stdlib
  `unittest`) with the tests directory as the top level, so the
  installed package is what they import. One result line per
  interpreter; each stage's output in `test-<python>-<stage>.log`.
- Exit 0 on success, 1 for a failed build or run, 2 when `pytest` finds
  no wheel. `validate --python` runs both on the exported tree.

## docgaps — public functions no doc names

```sh
uv run --project tools dktools docgaps [HEADER ...] [--names]
```

- Every function a public `src/` header declares (the public API as
  `coverage` counts it; suite and benchmark entry points aside) is looked
  for as a whole word in `docs/**/*.md` and `README.md`. One line per
  header with undocumented functions, most first, then totals;
  `--names` (or naming headers) lists the names.
- Without headers it also names module docs (`docs/modules/**/*.md`)
  that `docs/INDEX.md` does not link.
- A report for writing docs from: exit 0 (2 for a header that is not
  public).

## idioms — hand-written forms of what a shared helper does

```sh
uv run --project tools dktools idioms [FILE ...]
```

- One rule per idiom the platform has a shared form for, each naming
  the helper to use: `byte-shift-loop` (dkEndian.h), `inline-lcg` and
  `inline-xorshift64star` (dkRandom.h), `rand-64` (two 31-bit draws;
  dkSplitmix64Next), `qsort-comparator` (dkSort.h), `memcmp-err`
  (TEST_BYTES; `dktools testbytes` rewrites it), `div-ceil` (dkDivCeil),
  `encode-op-boxes` (a persist encodeOp's operands; persistOpEncodeBoxes),
  `databox-int-cast` (a databox's intmax_t / uintmax_t field cast to the
  64-bit type of the same sign; print it as is with PRIdMAX / PRIuMAX),
  `store-close-free` (a persist store's close then a hand-written free;
  persistStoreClose).
- Only code counts: comments and literals never match; preprocessor
  lines do. Each rule skips the file that defines its shared form.
- Exceptions are `PATH RULE REASON` lines in `tools/idioms-allow.txt`;
  a line without a reason or naming no rule is an input error (exit 2),
  and a line that no longer matches fails the gate, so the list stays
  true.
- Runs in `verify` and `validate` (the `idioms` line). Files default to
  the tracked `src/**/*.c` and `*.h`. Exits 0 clean, 1 for hits or
  stale allow lines.

## headers — every header compiles on its own

```sh
uv run --project tools dktools headers [--build DIR | --preset P] [-j N] [FILE ...]
```

- Compiles each tracked `src/**/*.h` (vendored trees aside) alone: a
  one-line unit that includes it, with the build's compile command for
  `src/datakit.c` (flags, include paths, defines; test builds' included),
  `-fsyntax-only` and no `-Werror`, so only errors fail. A header that
  relies on what its includer brought in first fails here.
- The database is DIR's `compile_commands.json` (`--build`, a verify
  tree's binary dir) or `build/PRESET`'s (`dktools configure`).
- Exceptions are `PATH REASON` lines in `tools/headers-allow.txt` (macro
  templates, which compile only after an includer defines their type
  macros); a line without a reason is an input error (exit 2), and a line
  whose header compiles on its own now, or names no checked header,
  fails, so the list stays true.
- Also checks every handle's `@handle` tag (see `handles`) and every
  struct body in a public header (see `structs`: a handle's or unnamed
  struct's body fails unless a `PATH NAME REASON` line in
  `tools/structs-allow.txt` names it, and a line naming no such body
  fails); the summary line counts the handles and bodies to fix.
- Runs in `verify` and `validate` after the build (the `headers` line).
  Exits 0 clean, 1 for a failing header, a stale allow line or a handle
  to fix.

## handles — whether a call can move each handle

```sh
uv run --project tools dktools handles [HEADER ...] [--list]
```

- A handle is an opaque struct typedef in a public header (no body
  there) or a typedef carrying a tag. The tag, `@handle: moves`,
  `returns` or `fixed`, sits in the comment on the typedef's line or the
  comment block just above it (docs/development/CONTRIBUTING.md,
  "Handles: Can a Call Move It?").
- Each tag is checked against every public call: prototypes, template
  prototypes under each type their type macro stands for (`DKS_TYPE` as
  `mds` and `mdsc`), and function pointers (a class's call table).
  `moves` needs a `T **` parameter; `returns` a call returning a `T`
  that takes one; `fixed` no `T **` parameter except an `out`, `*Out` or
  `*Ptr` one. `const T **`, `T *const *` and a `T **` return type are
  array reads, not moves.
- `--list` prints each handle with its tag and `T **` calls. Exits 0
  when every handle is tagged and consistent, 1 otherwise, 2 for a
  header that is not public. `headers` runs the same check.

## structs — struct bodies public headers show but should not

```sh
uv run --project tools dktools structs [HEADER ...] [--all]
```

- Sorts each struct with a body in a public header: `handle` (a public
  call returns it by pointer, so callers hold it as a handle and its
  fields belong behind an opaque typedef), `unnamed` (no public call
  names it, directly or through a struct one does) or `data` (a struct
  callers fill or read: configurations, results, statistics,
  iterators). Public calls are the prototypes, inline definitions and
  function pointers of every public header.
- Lists the `handle` and `unnamed` ones (`--all`: every one) and the
  totals. A report: exit 0, 2 for a header that is not public; `headers`
  runs it as a gate against `tools/structs-allow.txt`.
- Public headers are every `src/` header but a module's `*Internal.h` /
  `*Impl.h`, the vendored allocator's own, and test support (`ctest.h`,
  `dkTest*.h`, `*TestVectors.h`, `*TestCommon.h`), for every tool that
  reads the public API (`coverage`, `docgaps`, `handles`, `structs`).

## testchecks — TEST blocks that check nothing

```sh
uv run --project tools dktools testchecks [FILE ...]
```

- Lists each `TEST(...) { ... }` / `TEST_DESC(...) { ... }` block whose
  code names no check: none of `ERR`, `ERROR`, `TEST_BYTES`,
  `TEST_HEX`, `assert` or the failure count `err`, and no macro (in a
  scanned file or any header under src) whose body names one, directly
  or through another such macro. Such a block passes whatever the code
  under test does; only a crash could fail it.
- Blocks that measure are skipped: those named for it (the word perf,
  performance, speed, bench, benchmark, throughput, timing, latency,
  comparison, analysis, workload or head-to-head) and those that
  report a perf.h timing (`TIME_FINISH`). Comments and literals never
  count as a check.
- A block whose whole contract is that it runs (a printer, a free of
  NULL) is excepted by a `PATH :: NAME :: REASON` line in
  `tools/testchecks-allow.txt`, NAME as listed; a line without a reason
  is an input error (exit 2), and a line whose block is no longer
  listed fails the gate.
- Runs in `verify` and `validate` (the `checks` line). Files default
  to the tracked `src/**/*.c` and `*.h`. Exits 0 clean, 1 for listed
  blocks or stale allow lines.

## spell — misspellings in the sources, docs and tools

```sh
uv run --project tools dktools spell [--write] [FILE ...]
```

- Runs codespell (a locked tools dependency) over the tracked
  `src/**/*.c` and `*.h` (vendored `src/memshard` excluded), every
  `CMakeLists.txt`, `docs/**/*.md`, the root README and the tools'
  sources, tests and README, or over the files given. Prints each
  finding as `path:line: wrong ==> right`.
- Also flags pending-work markers (`TODO`, `FIXME`, `XXX`, `HACK`,
  `todo:`) in the C sources and Markdown docs, outside Markdown inline
  code: sources and docs state what is, and a gap is fixed before it is
  committed (or kept in the author's own notes).
- Words that are right here although codespell flags them
  (identifiers such as `aCount`, test strings, constant bytes such as
  ChaCha's `"nd 3"`) go one per line, lowercase, in
  `tools/spell-words.txt`.
- `--write` applies codespell's fix wherever it has exactly one
  suggestion and reports what remains. Read every finding first (a
  flagged word can be an identifier other code spells the same way) and
  review the diff.
- Exits 0 when nothing is (or remains) flagged, 1 when something is, 2
  for a missing file or a codespell failure.
- `verify` and `validate` run it over the exported tree as a gate (the
  `spell` line); in a tree that is not a git checkout the same patterns
  are matched by walking the directories.

## dropinclude — remove a header the language made unnecessary

```sh
uv run --project tools dktools dropinclude stdbool.h [--dry-run] [FILE ...]
```

- Deletes each `#include <HEADER>` line (a trailing comment allowed)
  from the tracked `src/**/*.c` and `*.h` (vendored `src/memshard`
  excluded), or from the files given. Quoted includes and commented-out
  lines stay.
- Prints each file's count. Exits 0, or 2 for a bad header name or a
  missing file.

## countof — element counts as countof(X)

```sh
uv run --project tools dktools countof [--dry-run] [FILE ...]
```

- In the tracked `src/**/*.c` and `*.h` (vendored `src/memshard`
  excluded), or the files given, where every byte is code (macro bodies
  included; comments and literals never): `sizeof(X) / sizeof(*X)` and
  `sizeof(X) / sizeof(X[0])` (also `sizeof((X)[0])` and unparenthesized
  `sizeof X / sizeof X[0]`) become `countof(X)`, X a name with any
  `.member`, `->member` and `[index]`; `COUNT_ARRAY(X)` becomes
  `countof(X)`.
- A ratio with any other divisor stays. `countof` (src/dkC23.h) refuses
  a pointer at compile time, so a site that stops compiling was counting
  a pointer's bytes.
- Prints each file's count. Exits 0, or 2 for a missing file.

## dropargs — cut every call of a function to the kept arguments

```sh
uv run --project tools dktools dropargs NAME --keep 0,2 --arity 5 [--dry-run] [FILE ...]
```

- Each call `NAME(a0, a1, a2, a3, a4)` in code becomes `NAME(a0, a2)`:
  the call-site half of retiring parameters (change the definition by
  hand). Arguments split at top-level commas; nested brackets, comments
  and literals stay whole.
- Every call must have `--arity` arguments, else the file is left alone
  and the call is listed (exit 1). Preprocessor lines are skipped, and
  a call of NAME nested in another's arguments is left as written.
- Files default to the tracked `src/**/*.c` and `*.h`.

## dupfuncs — function bodies written out more than once

```sh
uv run --project tools dktools dupfuncs [--min-tokens N] [FILE ...]
```

- Finds each file-scope function definition and compares bodies as
  tokens: comments and layout do not count, literals do.
- Lists every body of at least `--min-tokens` tokens (default 40) that
  is defined at two or more places, largest first, with each place's
  `path:line: name`. Each group is a candidate for one shared helper.
- Preprocessor lines are not code here, so functions a macro defines are
  not seen. Files default to the tracked `src/**/*.c` and `*.h`. Exits
  0 (the report is advice), or 2 for a missing file.

## irdups — functions that compile to the same code

```sh
uv run --project tools dktools irdups [--preset PRESET] [--min-instructions N] [-j N] [FILE ...]
```

- Compiles each source in `build/PRESET/compile_commands.json`
  (default `dev`; `dktools configure` makes it) to LLVM IR at
  `-O1 -fno-inline`, with the database's command for the file (the
  test target's when there is one) minus its output, optimisation,
  debug and sanitizer flags.
- Normalises each function body (metadata, attribute groups and value
  names go; locals and labels are numbered by first use; string
  constants read as their contents, not their numbering; callee and
  global names stay) and
  lists bodies of at least `--min-instructions` instructions (default 20) found under two or more function names, largest first. A header's
  static inline compiled into many units has one name and is not listed;
  a group whose names differ only in a type token (`I16`, `U64`,
  `Float`, ...) is marked "one type template" (instantiations of one
  source, counted apart). Code behind another target's `#if` (x86 SIMD
  on an arm64 host) is not compiled, so not compared.
- Finds what `dupfuncs --shape` cannot: copies that differ in spelling
  the optimizer erases (loop form, temporaries, macro versus function).
  Exits 0 (the report is advice), 2 for a missing database or a file it
  lacks.

## testbytes — byte comparisons in tests as `TEST_BYTES`

```sh
uv run --project tools dktools testbytes [--dry-run] [FILE ...]
```

- Each `if (memcmp(GOT, WANT, N) != 0) { ERR(FORMAT, ...); }` becomes
  `TEST_BYTES(err, GOT, WANT, N, FORMAT, ...);` (src/ctest.h), which
  counts the failure as ERR does and shows where the bytes differ.
- Each `if (memcmp(GOT, WANT, N) != 0) { printf(FORMAT "\n", ...); C++; }`
  becomes `TEST_BYTES(C, GOT, WANT, N, FORMAT, ...);`. The format must
  end in the newline, which TEST_BYTES supplies.
- Only those shapes: another condition, any other statement in the body
  or an `else` before or after it leaves the check as written, as do
  comments, literals and preprocessor lines.
- Files default to the tracked `src/**/*.c` and `*.h`. Prints each
  file's count. Exits 0, or 2 for a missing file.

## stdattr — `__attribute__` in the standard `[[...]]` form

```sh
uv run --project tools dktools stdattr [--dry-run] [FILE ...]
```

- Each `__attribute__((a, b(x)))` in code or a macro body becomes
  `[[gnu::a, gnu::b(x)]]` (`deprecated` is standard, `optnone` is
  `clang::optnone`, `aligned(N)` on an object is `alignas(N)`).
- Placement keeps the meaning: a standard attribute after a specifier
  attaches to the type, so one written after `static`/`inline` or after
  the declarator moves to the start of the declaration (the start of the
  body in a macro); right after `struct`/`union`/`enum` it stays.
- Then each use of an attribute macro (a macro whose body is only
  attributes, such as `DK_FN_CONST` or `cleaner(fn)`) that follows other
  specifiers moves to the front of its declaration, arguments included.
- A preprocessor line ends a declaration; text that is not one
  declaration's specifiers (macro calls without semicolons) is left and
  reported, as is an attribute after a struct body.
- An ignored attribute warns on both compilers, so the zero-warning
  build gate proves every placement.

## defconst — numeric #defines as typed constexpr constants

```sh
uv run --project tools dktools defconst [--dry-run] [FILE ...]
```

- Gives every `#define NAME <integer literal>` (and every `static
constexpr` integer constant) a verdict. It stays a macro when the
  preprocessor reads the name (`#if`/`#elif`/`#ifdef`/`#ifndef`/
  `#undef`/`#pragma`, `#` or `##` in a macro body, directly or through
  another macro an `#if` reads, in `src/` or the vendored `deps/`
  headers), when it is defined more than once, when it is in a public
  header, when nothing uses it (reported for a person), or when it is
  defined inside a function.
- Otherwise it becomes `static constexpr T NAME = LITERAL;` with `T` the
  literal's own C type (`int`, `unsigned`, `int64_t`, `uint64_t`), a
  trailing comment kept. A `constexpr` constant an `#if` reads goes back
  to `#define`, so a rerun with better rules converges.
- `--dry-run` reports. Exits 0, or 2 for a missing file.

## enumconst — one-constant enums as constexpr int

```sh
uv run --project tools dktools enumconst [--dry-run] [FILE ...]
```

- `enum { NAME = EXPR };`, the pre-C23 way to name an integer constant
  expression, becomes `constexpr int NAME = EXPR;` inside a function or
  macro body and `static constexpr int NAME = EXPR;` at file scope.
  `int` keeps the enum constant's type, so no arithmetic changes.
- Only code changes (macro bodies included; comments and literals
  never). Enums with a tag, a typedef, a fixed underlying type, several
  constants or no initializer stay.
- Prints each file's count. Exits 0, or 2 for a missing file.

## c23probe — what the compilers do with C23 features

```sh
uv run --project tools dktools c23probe [--compiler NAME ...]
```

- Compiles a fixed set of small programs with each named compiler
  (default: all) at `-std=gnu23 -O2` and prints a table: one row per
  probe, one column per compiler. Rerun it after a toolchain update.
- Attribute rows count the calls to `f` in `f(x) + f(x)`: `folds` means
  the compiler optimizes on the attribute, `no fold` that it accepts it
  but ignores it. `gnu::const` / `gnu::pure` are the baseline for
  `[[unsequenced]]` / `[[reproducible]]`.
- Construct rows (`nullptr`, `_BitInt`, `<stdcountof.h>`) say `yes`
  when the compiler accepts them; library rows (`#embed`, `<stdbit.h>`,
  `memalignment`, `printf %w64d`) build, run and compare the output.
- `(warns)` marks a probe that built with a diagnostic; `no: ...` shows
  the first diagnostic or the wrong output. Work files go under
  `build/dktools/c23probe/`. Exits 0.

## unusednames — retire `(void)name;` silencers for the C23 forms

```sh
uv run --project tools dktools unusednames [--dry-run] [FILE ...]
```

- In the tracked `src/**/*.c` and `*.h` (vendored `src/memshard`
  excluded), or the files given, each `(void)name;`, `DK_NOTUSED(name);`
  or `UNUSED(name);` goes and the name gets the C23 form:
  - a parameter the body never uses loses its name in the definition
    (the declaration keeps it), or stays named as `[[maybe_unused]]`
    when the comment directly above the definition names it (quoted,
    or identifier-shaped such as `bufferLen`; a plain word such as
    "key" in prose is not the parameter);
  - a parameter or local used only where a build may drop the code
    (`assert(...)` arguments, an `#if` branch), or a local only stored
    to, is declared `[[maybe_unused]]`;
  - a name read in code every build compiles keeps its declaration (the
    silencer was redundant).
- Assertions count as code a build may drop, except in test code
  (inside `#ifdef DATAKIT_TEST`, or a `*Test.c` / `datakit-test.c`
  file), where `ctest.h` refuses `NDEBUG` so they always compile.
- A read is certain unless the innermost call around it is a
  function-like macro the sources define (other than `assert`), which
  may store into it; a function only reads its arguments. One certain
  read in code every build compiles makes the silencer redundant.
- Stores (`x = ...`, `x += ...`, `x++`) are not reads; names after `.`
  or `->` are members. Code under every `#if` branch is part of the
  body (an `#if 0` branch is not code); an `#else` the silencer leaves
  empty goes with it.
- Leaves a silencer and lists it with the reason when a preprocessor
  line in the body names it, when a macro the body calls uses the name
  by itself (`ERR` ends with `err++` on whatever `err` is in scope;
  macros calling macros are followed), when the name has no parameter or local
  declaration the tool reads (a function pointer, a global), when a
  local is never used after its declaration (often a discarded call,
  `(void)g();`, which a person chooses), when the local is declared
  together with other names, when the silencer is not
  a statement of its own, or when it is outside a definition the tool
  reads. Macro bodies are never touched.
- Prints each file's count and every silencer left. Exits 0, or 2 for a
  missing file.

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
  `./build/perf/src/datakit-test test perfRegression > perf.baseline.tsv`.
  Paths are relative to `tools/` when run as shown; from the repository
  root, where the TSVs are generated, use
  `uv run --project tools dktools perfdiff perf.baseline.tsv perf.current.tsv`.

## tune — size limits and load factors measured on this machine

```sh
uv run --project tools dktools tune NAME [--families multimap,multiOrderedSet,multilist,multimapAtom,multidict,flexSlack] \
    [--mix [NAME:]KEY=SHARE[,KEY=SHARE...]]... [--baseline N] [--slower PCT] \
    [--runs N] [--reuse]
```

- Builds HEAD plus the changed tracked paths with the perf preset under
  `ROOT/tune/NAME`, then runs perfRegression's sweeps `--runs` times:
  the size-limit sweeps (`<family>Limits`: every operation at every limit
  from 256 B to 8 KB, 100,000 values, rows named `<op><limit>`) and
  multidict's load factor sweep (`multidictLimits`: 200% to 3200%, each
  operation summed over tables of 40k, 70k and 100k entries so a factor's
  numbers span a growth cycle; bytes are the tables' metered heap), and
  flex's growth slack sweep (`flexSlackLimits`: 0% to 25% of each growth,
  with a multidict, a multimap, a multilist and an ordered set written and
  read at each; bytes are each container's metered heap). The speed rule
  does not weigh bytes, so read the slack's byte changes too.
- Everything is read against the baseline: the setting the plain
  constructors use today, each family's own (2048 for the size limits, 512
  for the atom table's nodes, 800 for multidict, the compiled slack
  percent for flexSlack; `--baseline` sets one for every family), marked
  `*`.
  `--slower PCT` (default 15) is how much slower a win may make a mix or
  one of its operations: raise it where memory matters more than time.
  Per family and setting:
  - each operation's median ns/op and its change against the baseline (the
    family's header says which operations are reads and which writes);
  - the filled container's bytes and their change;
  - per workload mix, the mix's mean ns per operation against the
    baseline's (0.85 = 15% faster), with `[lowest-highest]` over the runs,
    each run compared with itself;
  - a `run-to-run spread` line: each operation's noise, so a change can be
    read against it.
- Mixes: `equal` (every operation equally often), `read80` / `write80`
  (80% reads, or 80% writes, each class's share split evenly over its
  operations), and any `--mix` (repeatable; `lookups:exists=90,insert=10`
  names one; shares by class, `read=95,write=5`, or by operation; an
  operation's own share wins over its class's).
- Per mix the answer is `keep` unless a limit is a clear win: 15% faster for
  the mix in every run, or 5% fewer bytes while under 15% slower in every
  run; never one that makes the mix, or any one operation it runs, 15% or
  more slower. A win names its limit and the call that applies it. A
  `keep` names the nearest limit and why it falls short (its slowest run's
  ratio, or the operation it slows), and the densest limit within 15%
  with its byte saving.
- `--reuse` reports again on the runs a tune of the same NAME measured
  (another `--baseline`, `--mix` or `--families`) without building or
  measuring. Also prints the machine's cache sizes (sysfs on Linux, sysctl
  on macOS). `result.txt` and `result.json` (per family: runs, noise,
  bytes, per-operation changes; per mix: the decision and every limit's
  ratio range) are kept in the work directory.
- The call is `dkTierSetDefaultLimit` (`src/dkTierLimit.h`), which each
  family's plain constructor (`multimapNew`, `multiOrderedSetNew`,
  `multilistNew`, every `multimapAtom` constructor) reads; for multidict,
  `multidictSetDefaultLoadFactors`
  (`src/multidict.h`), which `multidictNew` reads.

## blake3vectors — the BLAKE3 test vectors from the canonical JSON

```sh
curl -sSfLo test_vectors.json \
  https://raw.githubusercontent.com/BLAKE3-team/BLAKE3/master/test_vectors/test_vectors.json
uv run --project tools dktools blake3vectors test_vectors.json [--write]
```

- `src/hash/blake3TestVectors.h` holds every case of the BLAKE3 team's
  `test_vectors.json`: its input length and its three extended outputs
  (plain hash, keyed hash, derive key), whole, as hex. The blake3 tests
  compare digests, every output byte and output read from seek offsets
  across the 64-byte blocks against it.
- Without `--write` the command checks that the header is exactly what the
  JSON renders (exit 0, or 1 when it differs); `--write` rewrites it. Exit 2
  for a file that is not the vectors' JSON.

## matrix — every container's throughput and bytes per element

```sh
uv run --project tools dktools matrix NAME [--runs N] [--write] [--reuse]
```

- Builds HEAD plus the changed tracked paths and new files under src/
  with the perf preset under `ROOT/matrix/NAME`, then runs
  `datakit-test test containerMatrix` `--runs` times (default 3) with
  `DK_MATRIX=full`. Each container, element kind (small integers,
  integers across the 64-bit range, 16- and 256-byte strings) and size
  (1K, 100K and 1M; 256-byte strings to 100K) is measured phase by phase:
  create, read, update, delete. The test checks every result, so a wrong
  answer fails the run.
- Per container, kind, size and phase: operations per second at the
  median seconds over the runs, and the container's bytes per element
  after create. One markdown table per kind and size compares the
  containers, the fastest per phase and the smallest in bold.
- The report goes to `result.md` in the work directory and to stdout;
  `--write` also writes `docs/PERFORMANCE_MATRIX.md`, the generated
  companion of the hand-written `docs/CHOOSING_A_CONTAINER.md`.
  `--reuse` reports again on the runs an earlier matrix of the same NAME
  left, without building.
- A container joins the matrix as one driver in `src/containerMatrix.c`:
  make, create, read, update (optional), delete, bytes and count, and
  the kinds and largest size it takes. As a plain test,
  `containerMatrix` measures 100 elements of every kind and checks every
  result.

## perfcompare — a change's performance against a base revision

Builds a base revision and the working tree side by side and times them
on the same perfRegression benchmarks:

```sh
uv run dktools perfcompare NAME --base REV [--current REV] \
    [--modules M[,M...]] [--runs N] [--tolerance PCT] [--spread] \
    [--rename OLD=NEW ...] [--reuse] \
    [--bench-path PATH ...] [--compiler clang22|gcc15] [--root DIR] [-j N]
```

- `current` is HEAD plus every changed tracked path and every new file
  under `src/` (as `verify --all-changed`), or with `--current REV` that revision plus the
  benchmark sources: a commit timed against its parent while the working
  tree holds the next change. `base` is REV with the benchmark sources
  (`--bench-path`, default `src/perfRegression.c` and `.h`) taken from
  the working tree, so a benchmark added for the change also times the
  base. Both build only `datakit-test`, with the perf preset (gcc15:
  release), under `ROOT/perfcompare/NAME/{base,current}`.
- `datakit-test test perfRegression` runs on the two builds alternately,
  `--runs` times each (default 5; the first side alternates by round),
  limited to `--modules` through `DK_PERF_MODULES`. Each run is kept as
  `run-<side>-<round>.tsv`.
- Prints, and writes to `result.txt`, the median ns/op of each side per
  row (module, name, bytes) with the change in percent, and the rows only
  one side has. Each row's verdict compares the sides' runs with a
  rank-sum (Mann-Whitney U) test (counted over every split of the runs up
  to 8 a side, ties included; exact for untied samples of up to 40
  runs in all, else the normal approximation): `slower` / `faster` when the two-sided
  p is below 0.01, `~` when the runs do not tell the sides apart (a stray
  run moves one rank, not the verdict), `-` with fewer than five runs a
  side; `--tolerance` counts a row past it only with a `slower` verdict.
  A row whose bytes vary between one side's runs (a heap
  metered at the allocator) takes that side's median bytes; a row whose
  bytes differ between the sides (a metered heap the change grew or
  shrank) is paired by module and name and ends with `bytes -> N
(change)`. `--spread` adds each side's lowest and highest run per
  row: memory-bound rows (finds over tables larger than the caches) stray
  far more from run to run than compute-bound ones, and a change smaller
  than a row's own spread is noise.
- `--rename OLD=NEW` (repeatable) compares the base side's rows of module
  OLD with the current side's rows of NEW, for a benchmark module the
  change renamed. `--reuse` reports again on the runs an earlier
  perfcompare of the same NAME measured (another `--rename`,
  `--tolerance` or `--spread`), without building or running.
- Exits 0; 1 when `--tolerance PCT` is given and a row's median grew by
  more than PCT percent with a `slower` verdict, or when a build or run
  fails; 2 on an input error.

Use it for every before/after claim about a change: the benchmark lands
in perfRegression with the change, and the comparison is reproducible.

## profile — where perfRegression rows spend their time

```sh
uv run --project tools dktools profile NAME --modules M[,M...] [--rev REV] \
    [--top N] [--compiler clang22|gcc15] [--root DIR] [-j N]
```

- Builds HEAD plus every changed tracked path (as perfcompare's `current`),
  or with `--rev REV` that revision plus the benchmark sources (as
  perfcompare's `base`: profile a change and its base to see where the
  time moved), with the perf preset under `ROOT/profile/NAME`, then runs
  `datakit-test test perfRegression` once with `DK_PERF_MODULES` set to
  `--modules`, watched by the platform's sampling profiler: macOS
  `sample PID` (base system; attached as the run starts, stopped once it
  ends), Linux `perf record -g` (read back with `perf script`).
- Prints, and writes to `result.txt`, the `--top` functions by self samples
  (the sampled frame) and by inclusive samples (anywhere on the stack), as
  percents of all samples. The recording stays in the work directory.
- Exits 0; 1 when the build, the run or the profiler fails (or no sample
  was read); 2 on an input error.

Use it before a performance change to find what to change, and keep the
before/after claim to perfcompare.

## objdiff — did a function's machine code change, or only move?

```sh
uv run --project tools dktools objdiff NAME SYMBOL [SYMBOL ...] [--root DIR]
```

- Disassembles each function (with clang 22's `llvm-objdump`) from the
  two `datakit-test` binaries `dktools perfcompare NAME` built, since the
  perf builds link with LTO and the object files hold bitcode.
- Prints, per symbol, each build's start address with its offset in a
  64-byte line (`@0x100382830+48`), the instruction counts, and
  `identical` or `differs` plus a unified diff. Branch and call targets
  keep only their `<symbol+offset>`, page addresses read `ADDR`.
- Use it when a perfcompare row moves for code the change should not
  have altered: identical code at a new offset is placement, not a
  regression in the function.
- Exits 0 when every symbol is identical, 1 when one differs, 2 when a
  build or symbol is missing (a function inlined everywhere has no
  symbol of its own).
