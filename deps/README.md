# datakit dependencies

For ease of deployment, we are just copying dependencies here.

These local dependencies have been modified to work with our CMake build system if the upstream didn't support CMake originally.

The original sources are:

- varint: https://github.com/mattsta/varint
- xxhash: https://github.com/Cyan4973/xxHash (0.8.3, `dev` branch at 984ebb4; library sources and docs only, upstream's `tests/`, `cli/`, `fuzz/` and `.github/` are not vendored)
- lz4: https://github.com/lz4/lz4
- imath: https://github.com/mattsta/imath
