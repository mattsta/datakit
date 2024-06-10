#pragma once

#include <stddef.h>
#include <stdint.h>

size_t fibbufNextSizeBuffer(const size_t currentBufSize);
size_t fibbufNextSizeAllocation(const size_t currentBufSize);

#ifdef DATAKIT_TEST
int fibbufTest(int argc, char *argv[]);
#endif
