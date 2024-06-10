#pragma once

#include "databox.h"
#include "mds.h"
#include "str.h"
#include "strDoubleFormat.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef NDEBUG
/* Debug mode uses smaller so we can test growth edge cases easier */
#define DJ_BITMAP uint8_t
#else
#define DJ_BITMAP uint64_t
#endif

typedef struct djState {
    struct {
        mds *s;
        mds **ss;

        uint32_t ssCount;    /* number of elements in 'ss' */
        uint32_t ssCapacity; /* allocated extent of 'ss' */
    };
    size_t depth; /* which bit we are using inside boolIdx.{v,vBig} */
    size_t count; /* if 'count' is on an even integer, place key, else value */
    struct {
        /* Use 'v' by default, but if we need more bits than
         * 'v' can fit, allocate 'vBig' */
        union {
            DJ_BITMAP v[2];
            DJ_BITMAP *vBig;
        };

        /* Length of 'vBig' if 'vBig' is allocated.
         * When 'vBig' is not allocated, 'vBigExtent' is 0. */
        uint64_t vBigExtent;
    } boolIdx;
} djState;

#ifndef NDEBUG
/* minus 8 because vBig union is size 8 here but size 16 without debug */
_Static_assert(sizeof(djState) == 64 - 8, "Did you grow or shrink djState?");
#else
_Static_assert(sizeof(djState) == 64, "Did you grow or shrink djState?");
#endif

void djInit(djState *state);
void djInitWithBuffer(djState *state, mds *buf);
mds *djConsumeBuffer(djState *state, bool reset);
void djAppend(djState *dst, djState *src);
void djFree(djState *state);
mds *djFinalize(djState *state);
mds **djGetMulti(djState *state, size_t *ssCount);
mds **djFinalizeMulti(djState *state, size_t *ssCount);

/* Auto-detect whether closing list or map and do the right thing */
void djCloseElement(djState *state);

void djMapOpen(djState *state);
void djMapCloseElement(djState *state);
void djMapCloseFinal(djState *state);
void djArrayOpen(djState *state);
void djArrayCloseElement(djState *state);
void djArrayCloseFinal(djState *state);
void djTrue(djState *state);
void djFalse(djState *state);
void djNULL(djState *state);
void djString(djState *state, const void *data_, size_t len,
              const bool supportUTF8);
void djStringDirect(djState *state, const void *data, size_t len);
void djNumericDirect(djState *state, const void *data, size_t len);

/* Bonus: for generating python-style sets */
void djSetOpen(djState *state);
void djSetCloseElement(djState *state);
void djSetCloseFinal(djState *state);

void djBox(djState *state, const databox *const box);

#ifdef DATAKIT_TEST
int djTest(int argc, char *argv[]);
#endif
