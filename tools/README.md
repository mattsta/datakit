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
    [--root DIR] [-j N] PATH[=FILE]...
```

- Each `PATH` takes its working-tree content (or its deletion, when it
  is gone from the working tree); `PATH=FILE` stages `FILE`'s content
  at `PATH`. The tree is written through a private `GIT_INDEX_FILE`
  (the real index is never touched), exported with `git archive` into
  `ROOT/verify/NAME/src`, then configured and built there. `ROOT` is
  `--root`, else `$DKTOOLS_SCRATCH`, else `dktools/` under the system
  temp dir.
- `--tests` runs in parallel (`-j N`, default the CPU count);
  `--serial` runs one test at a time (the `*P` tests that share fixed
  temp paths) and is excluded from the parallel run. Both are
  whole-name ctest regexes, and one that matches nothing fails.
- The build covers the four preset targets unless `--target` is given.
  `allocaudit check --constructors` then runs on the exported tree
  with the tree's own dktools.
- Prints a result block (tree hash, build and warning count, ctest,
  sanitizer reports from the test output, allocaudit, failing test
  names, log paths), also kept in `ROOT/verify/NAME/result.txt`. Exits
  0 only when everything passed with 0 warnings and no sanitizer
  report, 1 otherwise, 2 on an input error (one stderr line).

Compilers and presets live in `src/dktools/toolchain.py`, the single
place builds get them from; the system `cc` is never used:

| name | programs | presets |
| --- | --- | --- |
| `clang22` (default) | `clang-22`, `clang++`, `llvm-ar`, `llvm-ranlib` from `--llvm-dir` / `$DKTOOLS_LLVM_DIR` (default: Homebrew LLVM) | `dev`, `debug` (CMakePresets.json) |
| `gcc15` | `gcc-15`, `g++-15`, overridden by `--gcc-cc`/`--gcc-cxx` or `$DKTOOLS_GCC_CC`/`$DKTOOLS_GCC_CXX` | `release` (`-DCMAKE_BUILD_TYPE=Release`, no preset) |

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
