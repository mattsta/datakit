#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct membound membound;
membound *memboundCreate(size_t size);
void memboundReset(membound *m);
size_t memboundCurrentAllocationCount(const membound *m);
bool memboundIncreaseSize(membound *m, size_t size);
bool memboundShutdown(membound *m);
bool memboundShutdownSafe(membound *m);
void *memboundAlloc(membound *m, size_t size);
void memboundFree(membound *m, void *p);
void *memboundRealloc(membound *m, void *p, int32_t newlen);

#ifdef DATAKIT_TEST
int memboundTest(int argc, char *argv[]);
#endif
