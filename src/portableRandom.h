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
__uint128_t portableRandom128(portableRandomState *r);

/* Note: these iterate in 8 or 16 byte units where total length of 'fill' is
 * (8 * count) bytes OR (16 * count) bytes! */
void portableRandomBy8(portableRandomState *r, void *fill, size_t count);
void portableRandomBy16(portableRandomState *r, void *fill, size_t count);

/* Call system random entropy source directly */
ssize_t portableRandomDirect(void *dst, size_t requestedLen);
