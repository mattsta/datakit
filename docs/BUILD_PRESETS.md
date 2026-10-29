# Build presets

`CMakePresets.json` defines five named configurations so that "performance vs
safety vs debug" sessions are reproducible and discoverable. Each preset
configures into its own `build/<name>/` directory so they coexist.

## Available presets

| Preset     | Build type     | Sanitizers      | CPU tuning     | Use when                                                                                                         |
| ---------- | -------------- | --------------- | -------------- | ---------------------------------------------------------------------------------------------------------------- |
| `debug`    | Debug          | ASAN + UBSAN    | default        | You want the loudest diagnostics, full debug symbols, no optimizer-eaten variables. Slowest.                     |
| `dev`      | Release (-O3)  | ASAN + UBSAN    | default        | Day-to-day development. Catches memory bugs at near-prod speed. Default for the repo.                            |
| `perf`     | Release (-O3)  | **none**        | `-mcpu=native` | Benchmarks and performance measurement only. Do NOT rely on this build to find memory bugs.                      |
| `safe`     | Release (-O3)  | ASAN + UBSAN    | default        | Intended for CI — strict correctness at acceptable speed.                                                        |
| `tsan`     | RelWithDebInfo | ThreadSanitizer | default        | Data races in the concurrency code. Several times slower; single-threaded suites gain nothing.                   |
| `coverage` | Release        | **none**        | default        | What `dktools coverage` builds: clang source coverage counters on every function.                                |
| `fuzz`     | RelWithDebInfo | ASAN + UBSAN    | default        | What `dktools fuzz` builds: the library instrumented for libFuzzer, and the `src/fuzz` targets (`datakit-fuzz`). |

Through dktools (which always builds with clang 22 and LLVM's archiver,
never the system compiler): `dktools verify --preset dev|debug|tsan|perf`
builds one preset and runs named tests; `dktools validate --presets
dev,debug,tsan` runs the whole matrix (quick tier by default, `--full` at
milestones; see [TESTING.md](development/TESTING.md)).

## Workflow

```sh
# Configure (once per preset; recreate after pulling CMakeLists changes):
cmake --preset perf

# Build (uses the configure dir created above):
cmake --build --preset perf

# Run a specific test directly:
./build/perf/src/datakit-test test blake3

# Or run via ctest (auto-discovers every registered test, 159 today):
ctest --preset perf
```

## Where builds live, and their garbage collection

Every build lives under one directory, `build/` (gitignored):

| Path                                          | Made by                    |
| --------------------------------------------- | -------------------------- |
| `build/<preset>/`                             | `cmake --preset <preset>`  |
| `build/dktools/verify/<name>/`                | `dktools verify NAME`      |
| `build/dktools/validate/<rev>-<configs>/`     | `dktools validate`         |
| `build/dktools/coverage/<rev>/` (and `.prev`) | `dktools coverage`         |
| `build/dktools/perfcompare/<name>/`           | `dktools perfcompare NAME` |
| `build/dktools/fuzz/<name>/` (corpus kept)    | `dktools fuzz NAME`        |

A dktools work directory carries a manifest (`.dkbuild.json`: kind, name,
revision, created, last used, size) and, while a run is using it, a lock
(`.dkbuild.lock`, the run's process id; a lock whose process is gone is
idle). Every dktools run that makes a work directory first removes idle
dktools builds unused for 14 days, then the least recently used until the
rest fit 40 GB (`DKTOOLS_BUILD_BUDGET_GB`, `DKTOOLS_BUILD_MAX_AGE_DAYS`). It
never removes a build in use, and never a preset build.

```sh
uv run --project tools dktools builds list              # every build: size, last use, state
uv run --project tools dktools builds gc --dry-run      # what a collection would remove
uv run --project tools dktools builds gc --budget-gb 10 # collect to a smaller budget
uv run --project tools dktools builds gc --legacy       # also the pre-build/ leftovers
```

`--legacy` removes builds made before this layout: `build-*` directories at
the repository root, the top-level files of an old single `build/` tree, and
the scratch root dktools used to keep in the system temp directory.
`--presets` also removes idle preset builds.

## Adding a new preset

Edit `CMakePresets.json` at the repo root. Inherit from the hidden `_base`
preset so the `binaryDir` and `CMAKE_EXPORT_COMPILE_COMMANDS` settings stay
consistent. Cache variables in your new preset override `_base`.

The five shipped presets cover the common axes (address/thread sanitizer
on/off × optimization on/off × native tuning on/off). Before adding a new
one, check whether toggling a single cache variable from an existing preset
on the command line is enough:

```sh
cmake --preset perf -DCMAKE_C_FLAGS_RELEASE="-O2"   # one-off variation
```

## CMake version

Presets require CMake ≥ 3.21. Earlier versions still work with manual
`cmake -DEnableASAN=OFF -DCMAKE_BUILD_TYPE=Release ...` invocations into a
build directory of your choice.
