# datakit dependencies

For ease of deployment, we are just copying dependencies here.

These local dependencies have been modified to work with our CMake build system if the upstream didn't support CMake originally.

The original sources are:

- varint: https://github.com/mattsta/varint
- xxhash: https://github.com/Cyan4973/xxHash (0.8.3, `dev` branch at 984ebb4; library sources and docs only, upstream's `tests/`, `cli/`, `fuzz/` and `.github/` are not vendored)
- lz4: https://github.com/lz4/lz4
- imath: https://github.com/mattsta/imath
- sha1: NetBSD's `sha1.c` (revision 1.6) and `<sys/sha1.h>` (revision 1.13), "SHA-1 in C" by Steve Reid, public domain

## Local patches

These changes are datakit's own and are not upstream. Carry them forward when re-syncing a package.

- sha1
  - `SHA1Final` writes the padding and bit length into `context->buffer` in place and transforms full 64-byte blocks. It does not stream the padding through `SHA1Update` one byte at a time.
  - `SHA1Update` adds `len >> 29` to the high count word on every call, not only on a low-word carry, so single updates of 2^29 bytes or more hash correctly. Its "completes a block" test is `len > 63 - j`, which cannot overflow.
  - `SHA1Update`'s definition takes `uint32_t len`, matching `sha1.h`, and uses `uint32_t` locals.
  - `sha1Test` is datakit's known-answer test (FIPS PUB 180-1 vectors, padding boundaries, chunked updates and one 2^29-byte update). It replaces the earlier stdin/file hashing harness.
  - `deps/sha1/CMakeLists.txt` is datakit's own build file.
- imath
  - `imath.c` allocates through the `IMATH_CALLOC`, `IMATH_REALLOC` and `IMATH_FREE` seams. The datakit build defines them before including `imath.c` textually from `src/bigmath-imath.c`; the standalone `libimath` target falls back to libc. `imath.c` must not include datakit headers.
  - `IMATH_NO_SANITIZE_UNSIGNED_WRAP` applies `no_sanitize("unsigned-integer-overflow")` under clang only, because GCC has no such sanitizer. `s_usqr` uses it.
  - `pi.c` parses its digit count and radix with checked `strtoimax`. It sizes its output buffer with `mp_int_string_len()` and checks the allocation and the conversion.
- xxHash
  - `deps/xxHash/CMakeLists.txt` is datakit's own build file; upstream has no root CMake build. It builds the C library without the C++-only `-Wno-extra-semi`.
  - Per the upgrade to 0.8.3, `xxhash.h` and `xxh_x86dispatch.{c,h}` are clang-formatted, and every other vendored file is byte-identical to upstream.
