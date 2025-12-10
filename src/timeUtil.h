#pragma once

#include "datakit.h"

uint64_t timeUtilNs(void);
uint64_t timeUtilUs(void);
uint64_t timeUtilMs(void);
uint64_t timeUtilS(void);

uint64_t timeUtilMonotonicNs(void);
uint64_t timeUtilMonotonicUs(void);
uint64_t timeUtilMonotonicMs(void);

#ifdef DATAKIT_TEST
int timeUtilTest(int argc, char *argv[]);
#endif
