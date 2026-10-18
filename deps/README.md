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
  - `mp_int_to_string` reserves the terminator before generating digits, so it never writes past `limit`. On `MP_TRUNC` the output is still terminated and holds the sign and the lowest-order digits that fit, at most `limit - 1` characters. The completeness check reads the remaining quotient after the digit loop. Upstream wrote `limit` digits plus the terminator.
  - `mp_rat_to_string` returns `MP_TRUNC` with the numerator still terminated when fewer than three characters remain for the slash, a denominator digit and the terminator.
  - `mp_rat_to_decimal` checks the space left before writing the sign, the point and the digits. On `MP_TRUNC` it leaves a terminated string, and it writes nothing when `limit` is 0.
    - Its fractional call gets all the space that is actually left. Upstream passed one character less.
    - A rounding carry out of the fractional digits is now absorbed into one fewer leading zero or into the integer part. Upstream printed 0.96 to one place as `0.10`; it now prints `1.0`.
    - An invalid rounding mode returns `MP_BADARG` after freeing its temporaries.
  - `gmp_compat.c`:
    - `impz_get_str` and `impq_get_str` return NULL for an unsupported radix, a failed allocation or a failed conversion. They use `size_t` lengths and apply digit case only up to the terminator.
    - `impq_set_str` and `impz_export` check their `malloc`.
    - The `_MSC_VER` `ssize_t` shim is removed (datakit builds with clang and GCC only). Word strides are `ptrdiff_t`.
    - `impz_export` and `impz_import` use `size_t` indices. They reject a zero word size, a byte total that overflows `size_t`, and a digit count beyond `mp_size`, and `impz_import` checks `mp_int_init_size`.
  - `mp_rat_to_decimal` counts leading zeros in an `mp_size`, the type of `prec`.
  - `pi.c` parses its digit count and radix with checked `strtoimax`. It sizes its output buffer with `mp_int_string_len()` and checks the allocation and the conversion.
- xxHash
  - `deps/xxHash/CMakeLists.txt` is datakit's own build file; upstream has no root CMake build. It builds the C library without the C++-only `-Wno-extra-semi`.
  - Per the upgrade to 0.8.3, `xxhash.h` and `xxh_x86dispatch.{c,h}` are clang-formatted, and every other vendored file is byte-identical to upstream.
