# Build presets

`CMakePresets.json` defines four named configurations so that "performance vs
safety vs debug" sessions are reproducible and discoverable. Each preset
configures into its own `build-<name>/` directory so they coexist.

## Available presets

| Preset  | Build type    | Sanitizers   | CPU tuning     | Use when                                                                                     |
| ------- | ------------- | ------------ | -------------- | -------------------------------------------------------------------------------------------- |
| `debug` | Debug         | ASAN + UBSAN | default        | You want the loudest diagnostics, full debug symbols, no optimizer-eaten variables. Slowest. |
| `dev`   | Release (-O3) | ASAN + UBSAN | default        | Day-to-day development. Catches memory bugs at near-prod speed. Default for the repo.        |
| `perf`  | Release (-O3) | **none**     | `-mcpu=native` | Benchmarks and performance measurement only. Do NOT rely on this build to find memory bugs.  |
| `safe`  | Release (-O3) | ASAN + UBSAN | default        | Intended for CI — strict correctness at acceptable speed.                                    |

## Workflow

```sh
# Configure (once per preset; recreate after pulling CMakeLists changes):
cmake --preset perf

# Build (uses the configure dir created above):
cmake --build --preset perf

# Run a specific test directly:
./build-perf/src/datakit-test test blake3

# Or run via ctest (auto-discovers all 73 registered tests):
ctest --preset perf
```

## Adding a new preset

Edit `CMakePresets.json` at the repo root. Inherit from the hidden `_base`
preset so the `binaryDir` and `CMAKE_EXPORT_COMPILE_COMMANDS` settings stay
consistent. Cache variables in your new preset override `_base`.

The four shipped presets cover the common axes (sanitizer on/off ×
optimization on/off × native tuning on/off). Before adding a new one, check
whether toggling a single cache variable from an existing preset on the
command line is enough:

```sh
cmake --preset perf -DEnableTSAN=ON     # one-off thread-sanitizer build
```

## CMake version

Presets require CMake ≥ 3.21. Earlier versions still work with manual
`cmake -DEnableASAN=OFF -DCMAKE_BUILD_TYPE=Release ...` invocations into a
build directory of your choice.
