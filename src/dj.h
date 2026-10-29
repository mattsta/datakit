#pragma once

#include "databox.h"
#include "mds.h"
#include "str.h"
#include "strDoubleFormat.h"

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
static_assert(sizeof(djState) == 64 - 8, "Did you grow or shrink djState?");
#else
static_assert(sizeof(djState) == 64, "Did you grow or shrink djState?");
#endif

/* A state starts zeroed; djInit or djInitWithBuffer then gives it its
 * first buffer.  Finalize, FinalizeMulti and Free leave it zeroed again,
 * so it can be initialised anew, and djFree on a zeroed state is a
 * no-op. */
void djInit(djState *state);
void djInitWithBuffer(djState *state, mds *buf);

/* Take the output written so far (single-buffer state; the caller frees
 * it with mdsfree).  With 'reset', writing continues into a fresh buffer
 * and the chunks taken, joined in order with what djFinalize returns, are
 * the whole document: a document can be streamed out as it is written.
 * Without 'reset' the state has no buffer left (djFree it). */
mds *djConsumeBuffer(djState *state, bool reset);

/* Move every buffer of 'src' to the end of 'dst' without copying; 'dst'
 * keeps writing into the last of them.  'src' is left zeroed.  The
 * output then spans several buffers: read it with djGetMulti or take it
 * with djFinalizeMulti (djFinalize refuses a multi-buffer state). */
void djAppend(djState *dst, djState *src);

/* Free every buffer and the state's own allocations. */
void djFree(djState *state);

/* Return the single output buffer (trailing comma removed), which the
 * caller frees with mdsfree. */
mds *djFinalize(djState *state);

/* A view of the output buffers in order, still owned by the state and
 * valid until its next write, append or free. */
mds **djGetMulti(djState *state, size_t *arrayCount);

/* Take the output buffers in order (trailing comma removed).  The caller
 * owns the array (zfree) and each buffer (mdsfree), even when there is
 * only one. */
mds **djFinalizeMulti(djState *state, size_t *arrayCount);

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
/* A quoted string, escaped: '"', '\\' and control bytes become \" \\ \b
 * \t \n \f \r or \u00XX.  With 'supportUTF8' valid UTF-8 is copied as it
 * is; without it the output is ASCII: each other character is \uXXXX (a
 * surrogate pair above U+FFFF).  Either way each byte that is not part
 * of a valid UTF-8 sequence becomes U+FFFD, so the output is always valid
 * JSON text. */
void djString(djState *state, const void *data_, size_t len,
              const bool supportUTF8);
/* A quoted string copied without escaping: the caller guarantees it holds
 * no '"', '\\' or control byte */
void djStringDirect(djState *state, const void *data, size_t len);
/* Bytes copied unquoted as a value (a number already formatted); quoted
 * in key position, since JSON keys are strings */
void djNumericDirect(djState *state, const void *data, size_t len);

/* Bonus: for generating python-style sets */
void djSetOpen(djState *state);
void djSetCloseElement(djState *state);
void djSetCloseFinal(djState *state);

/* Write a scalar box: integers and floats as numbers, bytes as escaped
 * strings, true/false/null as themselves.  In key position everything is
 * written as a string key (numbers and the three words quoted).  Returns
 * false and writes nothing for a box with no JSON form: a non-finite
 * float value, or VOID, ERROR, BYTES_VOID, BYTES_OFFSET, a container
 * reference, a container or a structure marker. */
bool djBox(djState *state, const databox *const box);

#ifdef DATAKIT_TEST
int djTest(int argc, char *argv[]);
#endif
