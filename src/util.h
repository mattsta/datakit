#pragma once

#include "../deps/sha1/sha1.h"
#include "datakit.h"

bool stringmatchlen(const char *pattern, int patternLen, const char *string,
                    int stringLen, int nocase);
bool stringmatch(const char *pattern, const char *string, int nocase);
uint64_t humanToBytes(const void *buf_, size_t len, bool *err);
bool bytesToHuman(void *buf, size_t len, uint64_t n);
void secondsToHuman(void *buf, size_t len, double sec);
bool pathIsBaseName(char *path);

void getRandomHexChars(uint8_t *p, uint32_t len);
bool getRandomHexCharsCounterInit(uint8_t seed[], uint8_t seedLen);
bool getRandomHexCharsCounter(uint8_t seed[SHA1_DIGEST_LENGTH],
                              uint64_t *counter, uint8_t *p, uint32_t len);

#ifdef DATAKIT_TEST
int utilTest(int argc, char **argv);
#endif
