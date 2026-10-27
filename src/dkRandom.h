#pragma once

#include "dkC23.h" /* UINT64_WIDTH */
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
 * A child of fork() never repeats the parent's draws: a pthread_atfork
 * handler makes it discard the output block it inherited and re-key
 * from the OS (see dkRandom.c).  That covers only forks that run atfork
 * handlers.  A child made without them (glibc _Fork(), a raw fork or
 * clone syscall), or any child if registering the handler failed,
 * re-keys at its next refill instead, when the pid check sees the new
 * pid.  Until then it can return the rest of the parent's buffered
 * block: up to 63 words the parent also returns.
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
#define DK_RANDOM_WORD_BITS UINT64_WIDTH

/* A uniform 64-bit value from this thread's generator. */
uint64_t dkRandom64(void);

/* A uniform value in [0, bound), exactly uniform at every bound.
 * bound == 1 returns 0.  bound == 0 is an empty interval and a caller
 * error: it returns 0 without dividing, increments
 * dkRandomZeroBoundTotal() and reports DK_DIAG_RANDOM_ZERO_BOUND
 * (dkDiag.h). */
uint64_t dkRandomBounded(uint64_t bound);

/* Process-wide count of dkRandomBounded(0) calls: a misuse diagnostic.
 * A nonzero count means some caller asked for a value in an empty
 * interval.  Counting costs nothing on valid calls; only the bound == 0
 * error path increments it. */
uint64_t dkRandomZeroBoundTotal(void);

/* A replayable stream: the 64-bit LCG with Knuth's MMIX constants (the
 * multiplier and increment PCG builds on).  For tests and benchmarks that
 * need the same values from the same seed, which dkRandom64 cannot give.
 * Advances '*state' and returns the new state; its low bits cycle
 * quickly, so small ranges should come from the high bits (>> 33 and the
 * like).  Reproducible data only: never for anything that must be
 * unpredictable. */
static inline uint64_t dkLcg64Next(uint64_t *state) {
    *state =
        *state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return *state;
}

#ifdef DATAKIT_TEST
int dkRandomTest(int argc, char *argv[]);
#endif
