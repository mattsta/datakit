#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

typedef struct portableRandomState {
    uint8_t i; /* these are uint8_t because they rely on one byte overflow */
    uint8_t j;
    int64_t remainingBytesBeforeRefuel;
    pid_t pid;
    uint8_t s[256];
} portableRandomState;

/* User space PRNG seeded by OS RNG.
 *
 * A re-key (portableRandom_stir, which any draw may run) does not wait
 * for the kernel pool: where getrandom() exists it reads without
 * blocking, and early in boot, before the pool is initialised, it keys
 * from /dev/urandom instead, and failing that from a time/pid mix.
 * Where getentropy() is the only source (macOS, OpenBSD), the re-key
 * calls it as is. */
void portableRandom_init(portableRandomState *r, pid_t pid);
void portableRandom_stir(portableRandomState *r, pid_t pid);
void portableRandom_add(portableRandomState *r, const uint8_t *key, size_t len);
uint32_t portableRandom32(portableRandomState *r);
uint64_t portableRandom64(portableRandomState *r);

/* Write `count` words to `out`: the same words that `count` calls of
 * portableRandom64 would return, but with one pid check and one budget
 * charge per chunk of words rather than per word.  A chunk that would
 * exhaust the refuel budget re-keys before it is generated, so no key
 * ever emits more than DK_PR_REFUEL_BYTES. */
void portableRandomFill64(portableRandomState *r, uint64_t *out, size_t count);

/* Bytes emitted between OS re-keys; installed by portableRandom_stir.
 * Each draw charges the bytes it produces against it. */
#define DK_PR_REFUEL_BYTES 100000

/* Read from the OS entropy source without blocking.  Returns the number
 * of bytes written, or -1, on every platform.  Where the source is
 * getentropy() (macOS, the BSDs), a request over 256 bytes fails with
 * -1; elsewhere it can succeed.  Use portableRandomDirectFull for
 * larger fills. */
ssize_t portableRandomDirect(void *dst, size_t requestedLen);

/* Fill `requestedLen` bytes from the OS CSPRNG.  Returns true only if
 * every byte was filled; on false the buffer must not be used.  Use
 * this for key material: the other functions here are an RC4-based
 * stream with known keystream biases, whose re-key falls back to a
 * time/pid mix when the OS source is unavailable.  The OS read is
 * non-blocking, so this fails rather than blocks if the kernel pool is
 * not yet initialised.  If the build detected no OS call (as happens
 * when cross-compiling), this reads /dev/urandom instead; on Linux that
 * device does not wait for the pool to initialise. */
bool portableRandomDirectFull(void *dst, size_t requestedLen);

#ifdef DATAKIT_TEST
int portableRandomTest(int argc, char *argv[]);
#endif
