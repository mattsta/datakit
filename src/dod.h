#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint64_t dod;
typedef int64_t dodVal;

/* There are two kinds of dod stream:
 *   - Raw streams, written with dodAppend(): delta-of-delta entries from
 *     bit 0. The caller keeps the first two values and passes them to
 *     dodGet() or dodReaderInit().
 *   - Writer streams, written with dodWrite(): a header holding the first
 *     two values (at most DOD_WRITER_HEADER_MAX_BITS bits), then the
 *     entries. dodRead(), dodReadAll(), dodInitFromExisting(),
 *     dodCloseWrites() and dodReaderInitFromWriter() read the header, so a
 *     writer stream decodes from the stream alone. To use dodGet() on one,
 *     start at the bit offset dodInitFromExisting() puts in 'usedBits'.
 * The stream never records how many values it holds. */
#define DOD_WRITER_HEADER_MAX_BITS 130

typedef struct dodWriter {
    /* Writer stream, owned by the writer: dodWrite() allocates it when NULL
     * and grows it with zrealloc as values arrive, keeping 'totalBytes'
     * current and room for readers to decode everything written. A caller
     * may preallocate it instead, zero-filled from the zmalloc family, with
     * 'totalBytes' set to its size. Free it with zfree(). */
    dod *d;

    /* If this writer is OPEN, then t[2] is the write helpers (the last two
     * values written).
     * If this writer is CLOSED, then t[2] is:
     *  - [0] - lowest complete value in 'd' (first)
     *  - [1] - highest complete value in 'd' (last)
     * A closed writer must not be written to again. */
    dodVal t[2];

    /* Number of elements written */
    /* (If < 2, the values are ONLY in t[2] and 'd' is untouched) */
    size_t count;

    /* Next bit offset to write inside 'd'. After dodInitFromExisting(), the
     * bit offset of the first delta-of-delta entry (the header's end). */
    size_t usedBits;

    /* Bytes allocated for 'd' (ignored while 'd' is NULL) */
    size_t totalBytes;
} dodWriter;

/* ====================================================================
 * Resumable Reader - O(1) sequential access
 * ==================================================================== */
typedef struct dodReader {
    size_t consumedBits; /* Bit position in stream */
    dodVal t0;           /* Second-to-last value (for delta-of-delta) */
    dodVal t1;           /* Last decoded value */
    size_t valuesRead;   /* Count of values decoded so far */
} dodReader;

/* Every int64_t sequence round-trips exactly, including INT64_MIN and
 * INT64_MAX neighbours and steps that do not fit in int64_t: the delta
 * math is modulo 2^64, so the stored delta-of-delta is the true one
 * reduced to its int64_t residue (equal to it whenever it fits). */
dodVal dodGet(const dod *d, size_t *consumedBits, dodVal originalStartVal,
              dodVal currentVal, size_t valueOffsetToReturn);
void dodAppend(dod *d, dodVal t0, dodVal t1, dodVal newVal,
               size_t *currentBits);

/* Value at index 'offset' (0 is the first value written) of an open or
 * closed writer; 'offset' must be below the number of values written.
 * With fewer than two values written it reads t[], and returns 0 for an
 * 'offset' past them. */
dodVal dodRead(dodWriter *const w, const size_t offset);
void dodWrite(dodWriter *const w, const dodVal val);

/* Read the header of writer stream 'd' (at least two values written):
 * t[] gets the first two values, 'usedBits' the bit offset of the first
 * entry and 'count' is set to 2. 'w->d' is not changed. */
void dodInitFromExisting(dodWriter *const w, const dod *d);

/* Switch 'w' to the closed form of t[] (first, last). */
void dodCloseWrites(dodWriter *const w);

/* Decode the first 'count' values of writer stream 'd' (at least two values
 * written) into 'vals', which must have room for at least 2. */
bool dodReadAll(const dod *w, uint64_t *vals, size_t count);

/* ====================================================================
 * dodReader API - O(1) resumable sequential access
 * ==================================================================== */

/* Initialize reader for a raw stream from its first two values (t0, t1) */
void dodReaderInit(dodReader *r, dodVal firstVal, dodVal secondVal);

/* Initialize reader for a writer's stream, open or closed: with two or more
 * values written it starts after the header, from the two values it holds */
void dodReaderInitFromWriter(dodReader *r, const dodWriter *w);

/* Read next value - O(1) operation. Advances iterator state. */
dodVal dodReaderNext(dodReader *r, const dod *d);

/* Peek at current value without advancing (returns t1) */
dodVal dodReaderCurrent(const dodReader *r);

/* Batch read - reads up to n values into out[], returns count read */
size_t dodReaderNextN(dodReader *r, const dod *d, dodVal *out, size_t n);

/* Get count of values remaining (requires knowing total count) */
size_t dodReaderRemaining(const dodReader *r, size_t totalCount);

#ifdef DATAKIT_TEST
int dodTest(int argc, char *argv[]);
#endif
