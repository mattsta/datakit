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
  while the load average is at or above the performance-core count.
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
    [--compiler clang22|gcc15] [--preset dev|debug] [--target T]... \
    [--root DIR] [-j N] (PATH[=FILE]... | --staged | --all-changed)
```

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
- Prints each file's count. Exits 0, or 2 for a token that is not a C
  identifier or a missing file.

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

## tune — size limits measured on this machine

```sh
uv run --project tools dktools tune NAME [--families multimap,multiOrderedSet,multilist] \
    [--mix [NAME:]KEY=SHARE[,KEY=SHARE...]]... [--baseline LIMIT] [--runs N] [--reuse]
```

- Builds HEAD plus the changed tracked paths with the perf preset under
  `ROOT/tune/NAME`, then runs perfRegression's size-limit sweeps
  (`<family>Limits`: every operation at every limit from 256 B to 8 KB,
  100,000 values, rows named `<op><limit>`) `--runs` times.
- Everything is read against the baseline: the limit the plain constructors
  use today (`--baseline`, default 2048, marked `*`). Per family and limit:
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
  `multilistNew`) reads.

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
  base. Both build only `datakit-test`, with the perf preset (gcc15:
  release), under `ROOT/perfcompare/NAME/{base,current}`.
- `datakit-test test perfRegression` runs on the two builds alternately,
  `--runs` times each (default 5; the first side alternates by round),
  limited to `--modules` through `DK_PERF_MODULES`. Each run is kept as
  `run-<side>-<round>.tsv`.
- Prints, and writes to `result.txt`, the median ns/op of each side per
  row (module, name, bytes) with the change in percent, and the rows only
  one side has.
- Exits 0; 1 when `--tolerance PCT` is given and a row's median grew by
  more than PCT percent, or when a build or run fails; 2 on an input
  error.

Use it for every before/after claim about a change: the benchmark lands
in perfRegression with the change, and the comparison is reproducible.
