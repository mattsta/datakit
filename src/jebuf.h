#pragma once

#include <stdbool.h>
#include <stddef.h>

size_t jebufSizeAllocation(size_t currentBufSize);
bool jebufUseNewAllocation(size_t originalSize, size_t newSize);

#ifdef DATAKIT_TEST
int jebufTest(int argc, char *argv[]);
#endif
