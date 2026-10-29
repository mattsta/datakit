#pragma once

/* AES-GCM internals shared by aesGcm.c and its hardware backends */

#include <stddef.h>
#include <stdint.h>

/* The AES key schedule (FIPS-197 5.2) in byte order: 11 round keys of 16
 * bytes for a 16-byte key, 15 for a 32-byte key (the caller checked the
 * length).  A backend whose instructions take round keys as byte vectors
 * fills them from here, so every backend expands keys one way. */
static constexpr size_t AES_GCM_ROUND_KEY_BYTES = 240;
void aesGcmKeyScheduleBytes(const uint8_t *key, size_t key_len,
                            uint8_t out[AES_GCM_ROUND_KEY_BYTES]);
