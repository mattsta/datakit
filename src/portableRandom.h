#pragma once

#include <stdint.h>
#include <unistd.h>

typedef struct portableRandomState {
    uint8_t i; /* these are uint8_t because they rely on one byte overflow */
    uint8_t j;
    int64_t remainingBytesBeforeRefuel;
    pid_t pid;
    uint8_t s[256];
} portableRandomState;

/* User space PRNG seeded by OS RNG */
void portableRandom_init(portableRandomState *r, pid_t pid);
void portableRandom_stir(portableRandomState *r, pid_t pid);
void portableRandom_add(portableRandomState *r, const uint8_t *key, size_t len);
uint32_t portableRandom32(portableRandomState *r);
uint64_t portableRandom64(portableRandomState *r);

/* Bytes emitted between OS re-keys; installed by portableRandom_stir.
 * Each draw charges the bytes it produces against it. */
#define DK_PR_REFUEL_BYTES 100000

/* Read from the OS entropy source without blocking.  Returns the number
 * of bytes written, or -1, on every platform. */
ssize_t portableRandomDirect(void *dst, size_t requestedLen);
