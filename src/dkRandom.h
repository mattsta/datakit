#pragma once

#include <limits.h>
#include <stdint.h>

/* Thread-local uniform random draws.
 *
 * dkRandom64() returns a full-width 64-bit value and dkRandomBounded()
 * a value in [0, bound), both from a generator owned by the calling
 * thread (a portableRandomState seeded from the OS), so draws never
 * contend and never race on shared state.
 *
 * These replace the `(random() << 32) | random()` then `% bound` idiom,
 * which yields only 62 random bits (bits 31 and 63 are always zero),
 * cannot reach half the residues of a bound that is a multiple of 2^32,
 * is modulo-biased, and reads process-global libc state.
 *
 * A forked child never repeats the parent's draws: it discards the
 * output block it inherited and re-keys from the OS (see dkRandom.c).
 *
 * Not for key material: the generator is an RC4-based stream with known
 * keystream biases (see portableRandom.h).  Use
 * portableRandomDirectFull() for secrets.
 *
 * Not for replayable test streams: draws are OS-seeded and cannot be
 * reproduced from a printed seed, so tests that replay via
 * srandom(seed) keep using random().
 *
 * The prefix differs from portableRandom* because ownership differs:
 * portableRandom* callers own their state; this module owns one state
 * per thread. */

/* The source word's width in bits.  Published so a caller iterating
 * over a draw's bits — an every-bit oracle, a mask — never spells the
 * literal, and so the width moves in one place if the source ever
 * widens. */
#define DK_RANDOM_WORD_BITS (sizeof(uint64_t) * CHAR_BIT)

/* A uniform 64-bit value from this thread's generator. */
uint64_t dkRandom64(void);

/* A uniform value in [0, bound), exactly uniform at every bound.
 * bound == 1 returns 0.  bound == 0 is an empty interval and a caller
 * error: it returns 0 without dividing and increments
 * dkRandomZeroBoundTotal(). */
uint64_t dkRandomBounded(uint64_t bound);

/* Process-wide count of dkRandomBounded(0) calls, for tests. */
uint64_t dkRandomZeroBoundTotal(void);

#ifdef DATAKIT_TEST
int dkRandomTest(int argc, char *argv[]);
#endif
