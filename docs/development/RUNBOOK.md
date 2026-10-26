# Change runbook

The checklists every change follows, human or agent. Each step is a
`dktools` command (`uv run --project tools dktools ...`, details in
[tools/README.md](../../tools/README.md)). The commands are the
interface: never run a formatter, a checker or the test suite by hand
when a command covers it. If a step you need has no command, add one to
`tools/` (Python, with tests) before relying on it.

## Before you start

- Builds live under `build/` only (`build/<preset>` for CMake presets,
  `build/dktools/...` for dktools runs); `dktools builds list` shows
  them, and `dktools builds gc` collects idle ones. Never create build
  directories anywhere else.
- Compilers: clang 22 (Homebrew LLVM, with `llvm-ar`/`llvm-ranlib`) and
  gcc 15. Never the system `cc`. dktools picks them
  (`src/dktools/toolchain.py`).
- One change at a time: finish, verify and commit it before the next.

## The change loop

For every change:

1. **Test first.** Add or extend the test that pins the behaviour:
   - a unit or differential test per primitive;
   - an independent oracle where one exists;
   - asserts on the contract (round trips, exact values), never only
     "output is non-empty";
   - modest loop counts. Benchmarks inside tests stay smoke-sized.
2. **Implement.**
3. **Format:** `dktools format write`. It formats the files changed from
   HEAD plus untracked ones: clang-format for C, prettier for Markdown,
   ruff for Python.
4. **Verify:** test on clang 22 and build on gcc 15 in one run, naming
   only the tests the change touches:

   ```sh
   uv run --project tools dktools verify NAME --compiler clang22 --also-build gcc15 \
       --tests '^(testA|testB)$' PATH...
   ```

   Name every changed file (or use `--all-changed`, or stage and use
   `--staged`). verify builds HEAD plus exactly those files, so other
   uncommitted work cannot affect the result. gcc 15 compiles and links
   the same tree without running tests; the two-compiler test run is
   validate, at a milestone. Run one verify at a time.

5. **Watch the time.** The `time` line gives each step's wall time; keep
   a cycle short by naming fewer tests or splitting the change.
6. **Read the result block.** It passes only when every line is clean:
   - `build ok, 0 warning lines`, and the `also` line the same;
   - `ctest ok`;
   - `sanitizer ok`;
   - `alloc`, `atomic`, `output` and `format` all `ok`.

   Any `FAILED` line blocks the commit, whatever caused it.

7. **If the change can affect speed**, compare it with HEAD on the perf
   preset (no sanitizers):

   ```sh
   uv run --project tools dktools perfcompare NAME --base HEAD --modules M --runs 15
   ```

   Two identical builds differ by up to about ±8%, so use 15 runs and
   report before/after numbers. A regression is fixed, not explained.

8. **If the change adds or removes code paths**, check that the tests
   reach them:

   ```sh
   uv run --project tools dktools coverage --detail src/FILE.c
   ```

   `--reuse --rev REV` reads an earlier run without rebuilding.

9. **If the change reads then writes the same container and key**, run
   `dktools fusionscan PATH...` and fold any pair where one search can do
   the work of two.
10. **Commit** with the message in a file (a subject, then `What:`,
    `Why:` and `How:` paragraphs):

    ```sh
    uv run --project tools dktools commit NAME -F FILE PATH...
    ```

    It commits exactly those paths, only if verify `NAME` passed and each
    path is byte-for-byte what it built, and only after the leakage gate.

Changes to `tools/` itself also run the tools' own tests:

```sh
uv run --project tools python -m unittest discover -s tools/tests
```

## Milestones

At the end of a series of changes, validate the commit:

```sh
uv run --project tools dktools validate          # quick tier
uv run --project tools dktools validate --full   # also the slow tests
```

validate exports the commit and builds clang 22 dev, clang 22 debug and
gcc 15 release. It runs every test plus every gate: allocation
ownership, atomic ordering, library output, formatting and leakage.
Read `summary.md` in its work directory: `PASS` on the first line, and
every section clean. Never pipe validate or coverage into `head` (the
pipe kills the run while the exit status still looks successful). Start
no other builds while it runs.

After a milestone, refresh the coverage baseline:
`dktools coverage --update-baseline`. Add `--accept-regressions` only
for a drop you have explained.

## Finding problems

| Question                                 | Command                                                     |
| ---------------------------------------- | ----------------------------------------------------------- |
| Which section of a test is slow?         | `dktools testtime build/dev/src/datakit-test NAME`          |
| What did a change do to speed?           | `dktools perfcompare NAME --base REV --modules M --runs 15` |
| Which lines and branches do tests miss?  | `dktools coverage --detail FILE`                            |
| Which public API do tests never call?    | `dktools coverage` (the untested-API list)                  |
| Read-then-write pairs to fold?           | `dktools fusionscan`                                        |
| Is a file formatted?                     | `dktools format check PATH...`                              |
| What builds exist, and which are in use? | `dktools builds list`                                       |

A sanitizer failure that shows up about once in twenty runs depends on
memory contents or layout. Repeat only that test until it fails. Never
reason about it from addresses.
