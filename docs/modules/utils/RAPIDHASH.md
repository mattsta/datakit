# rapidhash: fast non-cryptographic 64-bit hashing

**Header**: `rapidhash.h`

rapidhash V3 hashes bytes to 64 bits at tens of GB/s. It is for hash tables,
sharding, content fingerprints and checksums against accidental corruption.
It is not a MAC: anyone who can choose inputs can make collisions. For
tamper evidence use keyed BLAKE3 or HMAC ([Hashes, HMAC, and the flex
envelope](../../HASH_AND_ENVELOPE.md)).

## Variants are different hash functions

`rapidhash`, `rapidhashMicro` and `rapidhashNano` agree only on short
inputs (up to 48 bytes for Nano, up to 80 for Micro against Main); past that
they produce different values by design. Pick one variant for anything that
stores or compares hashes (a table, a fingerprint on disk, a routing key) and
keep it for the life of that data.

| Variant          | Bulk stride    | Best for                                      |
| ---------------- | -------------- | --------------------------------------------- |
| `rapidhash`      | 7 lanes, 112 B | Unknown or long inputs; the default           |
| `rapidhashMicro` | 5 lanes, 80 B  | Keys of one cache line to a few hundred bytes |
| `rapidhashNano`  | 3 lanes, 48 B  | Inputs that fit 48 bytes; smallest code       |

perfRegression's `rapidhash` rows measure each variant, one-shot and
streaming, at sizes from 8 bytes to 1 MB.

## Calls

Each variant has the same set, in FAST and PROTECTED mixing:

```c
uint64_t rapidhash(const void *key, size_t len);
uint64_t rapidhashWithSeed(const void *key, size_t len, uint64_t seed);
uint64_t rapidhashWithSecret(const void *key, size_t len, uint64_t seed,
                             const uint64_t secret[RAPIDHASH_SECRET_LEN]);
/* rapidhashMicro, rapidhashMicroWithSeed, rapidhashMicroWithSecret,
 * rapidhashNano, rapidhashNanoWithSeed, rapidhashNanoWithSecret */

uint64_t rapidhashProtected(const void *key, size_t len);
uint64_t rapidhashProtectedWithSeed(const void *key, size_t len, uint64_t seed);
uint64_t rapidhashProtectedWithSecret(
    const void *key, size_t len, uint64_t seed,
    const uint64_t secret[RAPIDHASH_SECRET_LEN]);
/* rapidhashMicroProtected, rapidhashMicroProtectedWithSeed,
 * rapidhashMicroProtectedWithSecret, rapidhashNanoProtected,
 * rapidhashNanoProtectedWithSeed, rapidhashNanoProtectedWithSecret */
```

- **Seed**: a different seed gives an unrelated hash function of the same
  variant; use one to keep an attacker who knows the default from
  precomputing collisions.
- **Secret**: the 8 words the mixing uses. `rapidhashGetDefaultSecret()`
  returns the default (copy it before changing it).
  `rapidhashMakeSecret(seed, secret)` derives words 0..3 from a seed and
  fills 4..7 from the default; overwrite those too when tables must differ
  in every word.
- **PROTECTED** mixing XORs the multiply result into its inputs, which
  resists the algebraic-invariant attacks on wyhash V1, at about 10% of
  throughput. Its values differ from FAST's: pick a mode as you pick a
  variant. It is still not a MAC.

Output is the same on little- and big-endian hosts. Nothing allocates.

## Streaming

```c
rapidhashCtx ctx;
rapidhashStreamInit(&ctx, RAPIDHASH_VARIANT_MAIN, RAPIDHASH_MODE_FAST, seed,
                    nullptr /* default secret */);
rapidhashStreamUpdate(&ctx, part1, len1);
rapidhashStreamUpdate(&ctx, part2, len2);
const uint64_t h = rapidhashStreamFinal(&ctx);
/* == rapidhashWithSeed(part1 || part2) */
```

The stream gives the one-shot value of the concatenated input for its
variant and mode, in any chunking. Final consumes the context: Init again
before reusing it. Streaming costs several times the one-shot per byte (each
Update buffers and dispatches per stride); hash in one call when the bytes
are already together.

## Main-variant loop shapes

The Main variant compiles two loops with identical output: COMPACT (one
112-byte stride per pass) and UNROLLED (two per pass, faster on long inputs).
By default each call picks UNROLLED for inputs longer than
`rapidhashAutoUnrollThreshold()` bytes (224). `rapidhashSetForceImpl` pins one
for benchmarks and differential tests (`rapidhashGetForceImpl` reads it back);
set it before other threads hash, since it is a plain process global.
Streaming ignores it.
