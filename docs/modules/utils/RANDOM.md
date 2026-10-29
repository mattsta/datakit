# Random numbers: which source for which job

**Headers**: `dkRandom.h`, `portableRandom.h`

datakit has three kinds of random source. Pick by what the numbers are for:

| Job                                                                          | Use                                                                                         |
| ---------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| Keys, nonces, tokens, anything an attacker must not predict                  | `portableRandomDirectFull(buf, len)` (the OS CSPRNG)                                        |
| Everyday randomness: sampling, jitter, random eviction, shuffles, hash seeds | `dkRandom64()`, `dkRandomBounded(n)` (per thread)                                           |
| The same values from the same seed: tests, benchmarks, generated data        | `dkSplitmix64Next(&seed)`, `dkSplitmix64Bounded(&seed, n)` and the other replayable streams |

## Secrets: the OS source

```c
#include "portableRandom.h"

uint8_t key[32];
if (!portableRandomDirectFull(key, sizeof(key))) {
    return false; /* the buffer must not be used */
}
```

`portableRandomDirectFull` fills every byte from the OS (`getrandom`,
`getentropy`, or `/dev/urandom` where the build found no call) and is true
only when it did. It never blocks: early in boot, before the kernel pool is
ready, it fails instead. `portableRandomDirect(dst, len)` is one OS read,
returning the bytes written or -1 (over 256 bytes fails where the source is
`getentropy`, on macOS and the BSDs).

## Everyday randomness: one generator per thread

```c
#include "dkRandom.h"

const uint64_t word = dkRandom64();             /* 64 uniform bits */
const uint64_t slot = dkRandomBounded(buckets); /* [0, buckets), exactly uniform */
```

Each thread draws from its own generator, keyed from the OS, so draws never
contend or race. A child of `fork()` re-keys rather than repeating its
parent's values (immediately where fork handlers run; at its next refill
otherwise). `dkRandomBounded(0)` asks for a value from an empty interval: it
returns 0, counts the call (`dkRandomZeroBoundTotal()`) and reports
`DK_DIAG_RANDOM_ZERO_BOUND`.

These replace the `(random() << 32) | random()` and `% bound` idiom, which
gives 62 random bits, a modulo bias, and shared libc state. They are not for
secrets: the generator is an RC4-based stream with known biases.

The generator underneath is `portableRandomState`, usable directly when a
caller wants a state of its own: `portableRandom_init(&r, getpid())` keys
it, `portableRandom32` / `portableRandom64` draw, `portableRandomFill64(&r,
out, n)` fills an array, `portableRandom_stir(&r, pid)` re-keys from the OS
(draws do it every `DK_PR_REFUEL_BYTES` bytes) and `portableRandom_add(&r,
bytes, len)` mixes caller bytes into the key.

## Replayable streams

```c
uint64_t seed = 42; /* print it with any failure: the run replays */
for (size_t i = 0; i < n; i++) {
    const uint64_t key = dkSplitmix64Next(&seed);
    const uint64_t pick = dkSplitmix64Bounded(&seed, 100); /* [0, 100) */
}
```

The same seed gives the same values on every platform, which `srand` and
`rand` do not (each libc has its own generator, and `rand` has 31 bits).

| Stream                                               | Notes                                                                     |
| ---------------------------------------------------- | ------------------------------------------------------------------------- |
| `dkSplitmix64Next(&s)`, `dkSplitmix64Bounded(&s, n)` | the default: any seed (zero included), full-quality bits, exact bounds    |
| `dkLcg64Next(&s)`                                    | Knuth's MMIX LCG: fast; its low bits cycle quickly, so take high bits     |
| `dkXorshift64Next(&s)`, `dkXorshift32Next(&s)`       | Marsaglia's xorshift; a zero seed stays zero                              |
| `dkXorshift64StarNext(&s)`                           | xorshift64\*: the multiply repairs the low bits                           |
| `dkSplitmix64Mix(x)`                                 | splitmix's finalizer alone: a 64-bit bijection, a good integer hash mixer |

`str.h`'s `splitmix64()` and `xorshift64star()` are the same streams as
exported functions. None of these is unpredictable: never use them for
anything security-relevant.
