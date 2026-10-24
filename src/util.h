#pragma once

#include "../deps/sha1/sha1.h"
#include "datakit.h"

bool stringmatchlen(const char *pattern, int patternLen, const char *string,
                    int stringLen, int nocase);
bool stringmatch(const char *pattern, const char *string, int nocase);
/* Parse "<digits>[ ...][unit]" from the first 'len' bytes of 'buf_' (or up
 * to a NUL) into a byte count. Units are case-insensitive: b; k/kb, m/mb,
 * g/gb, t/tb, p/pb, e/eb (powers of 1000); kib through eib (powers of 1024);
 * no unit means bytes. A missing number, an unknown unit, a Z or Y unit, or
 * any result above UINT64_MAX is refused: the return is 0 and '*err' (if
 * non-NULL) is set to true. On success '*err' is set to false. */
uint64_t humanToBytes(const void *buf_, size_t len, bool *err);
/* Format 'n' bytes into 'buf' as "N B" below 1 KiB, else with five
 * decimals in the largest binary unit (KiB through EiB) that keeps the
 * value at or above 1. Every uint64_t fits, so it always returns true. */
bool bytesToHuman(void *buf, size_t len, uint64_t n);
void secondsToHuman(void *buf, size_t len, double sec);
bool pathIsBaseName(char *path);

/* 'len' bytes from the OS CSPRNG; when the OS source fails (it never
 * blocks, so before the kernel pool is ready) from a time / pid / rand()
 * mix, which is predictable.  For identifiers and seeds, not key
 * material: key material uses portableRandomDirectFull and refuses on
 * failure. */
void getRandomBytes(uint8_t *p, uint32_t len);
/* getRandomBytes as 'len' lowercase hex characters (4 bits each) */
void getRandomHexChars(uint8_t *p, uint32_t len);
bool getRandomHexCharsCounterInit(uint8_t seed[], uint8_t seedLen);
bool getRandomHexCharsCounter(uint8_t seed[SHA1_DIGEST_LENGTH],
                              uint64_t *counter, uint8_t *p, uint32_t len);

#ifdef DATAKIT_TEST
int utilTest(int argc, char **argv);
#endif
