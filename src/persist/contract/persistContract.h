/* persistContract.h - one contract every persistOps descriptor passes
 *
 * persistContractTest drives each registered descriptor through the
 * contract below at several sizes and seeds, under every build
 * configuration (sanitizers watch the corruption cases), and prints one
 * table row per type for operators.  The contract text is frozen: a
 * type that cannot meet it is fixed, never excused.
 *
 *   1. Round trip: snapshot -> restore -> snapshot gives identical
 *      bytes, an equal count(), and validate() true when the descriptor
 *      has one.  The empty structure round-trips too.
 *   2. WAL: the generator records its starting structure and every
 *      mutation it makes, encoded through encodeOp as a WAL writer
 *      would.  Applying those ops through applyOp to
 *        - restore(the starting snapshot),
 *        - newEmpty(), when its snapshot equals the starting snapshot,
 *        - restore(snapshot of the first half replayed), then the
 *          second half,
 *      each gives the live structure's snapshot bytes.
 *   3. Damage: every truncation and a seeded set of single-byte
 *      corruptions of a snapshot either make restore return NULL or
 *      give a structure that itself meets (1).  Never a crash.
 *   4. Strings: a type that stores strings keeps numeral-looking
 *      strings ("22", "007", "1.5", "-3", 128-bit numerals) as the same
 *      bytes through snapshot, restore and WAL replay.
 *
 * A type registers a persistContractType (in its own file under
 * src/persist/contract/) and adds it to the registry in
 * persistContractTest.c. */

#pragma once

#include "../../databox.h"
#include "../../persist.h"

typedef struct persistContractLog persistContractLog;

/* What the harness asks a generator to build. */
typedef struct persistContractBuild {
    uint64_t seed;
    size_t size; /* about this many elements */
    /* When non-NULL, every one of these BYTES values is stored in the
     * structure (as a value the type keeps as bytes). */
    const databox *strings;
    size_t stringCount;
    persistContractLog *log;
} persistContractBuild;

/* Generators call this once, right after constructing the structure and
 * before mutating it: the harness snapshots the starting structure, as
 * a WAL writer's creation record would. */
void persistContractStart(persistContractLog *log, const void *structure);

/* Generators call this after each mutation of the live structure, with
 * the args and argc that ops->encodeOp takes for `op`. */
void persistContractRecord(persistContractLog *log, persistOp op,
                           const void *args, size_t argc);

typedef struct persistContractType {
    const persistOps *ops;
    /* Builds a structure as described by `b` through the type's own API,
     * calling persistContractStart once and persistContractRecord for
     * each mutation.  Returns the live structure (freed with ops->free). */
    void *(*build)(const persistContractBuild *b);
    /* True iff the structure holds `bytes` (a BYTES box) as BYTES with
     * the same content.  NULL for types that store no strings (4 is then
     * not applicable). */
    bool (*holdsBytes)(const void *structure, const databox *bytes);
} persistContractType;

/* A seeded value of any databox kind a container stores: small and full
 * range signed/unsigned, 128-bit signed/unsigned, double, float, true,
 * false, null and bytes (some of them digit strings).  Bytes point into
 * `buf`, so `buf` must outlive the value; `out` holds 128-bit values
 * itself. */
void persistContractValue(uint64_t *rng, databoxBig *out, uint8_t *buf,
                          size_t bufLen);

/* True iff `got` is a bytes box (plain or embedded) with the content of
 * the bytes box `want`; numbers never match.  For holdsBytes. */
bool persistContractSameBytes(const databox *got, const databox *want);

#ifdef DATAKIT_TEST
int persistContractTest(int argc, char *argv[]);
#endif
