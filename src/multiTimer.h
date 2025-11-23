#pragma once

#include "multimap.h"
#include <stdint.h>

typedef struct multiTimer multiTimer;

typedef uint64_t multiTimerId;
typedef int64_t multiTimerUs;
typedef int64_t multiTimerSystemMonotonicUs;

typedef bool multiTimerCallback(multiTimer *t, multiTimerId id,
                                void *clientData);

/* Init / Cleanup */
multiTimer *multiTimerNew(void);
void multiTimerFree(multiTimer *t);
bool multiTimerInit(multiTimer *t);
void multiTimerDeinit(multiTimer *t);

size_t multitimerCount(multiTimer *t);

/* Export our timer calculator */
multiTimerSystemMonotonicUs multiTimerGetNs(void);
multiTimerSystemMonotonicUs multiTimerGetUs(void);

/* Timer processing */
void multiTimerProcessTimerEvents(multiTimer *t);

/* Lowest entry in scheduled timer map. */
multiTimerSystemMonotonicUs multiTimerNextTimerEventStartUs(multiTimer *t);
multiTimerUs multiTimerNextTimerEventOffsetFromNowUs(multiTimer *t);

/* Timers */
multiTimerId multiTimerRegister(multiTimer *t, uint64_t startAfterMicroseconds,
                                uint64_t repeatEveryMicroseconds,
                                multiTimerCallback *cb, void *clientData);
bool multiTimerUnregister(multiTimer *t, multiTimerId id);
bool multiTimerStopAll(multiTimer *t);

#ifdef DATAKIT_TEST
int multiTimerTest(int argc, char *argv[]);
#endif
