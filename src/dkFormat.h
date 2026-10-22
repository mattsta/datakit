#pragma once

/* dkFormat - identity and version of the byte formats datakit writes
 *
 * Bytes that leave a process (files, the network, a caller holding a
 * serialization) carry a stamp naming their format, its version and its
 * feature bits, so a reader never parses a format it does not understand:
 * it refuses with a typed result naming what it found.  In-memory structures
 * carry no stamps; the running build is the format it runs.
 *
 * Layers that stamp:
 *   - each data structure's own encoding (its snapshot payload and WAL op
 *     encodings), id = its persistType;
 *   - the persistence container (snapshot and WAL file framing);
 *   - wire protocols (cluster mailbox frames).
 * The stamp travels once, where the bytes leave: in a persist file header,
 * in a frame header, or as the prefix of a standalone serialization.
 *
 * Versions change only for incompatible encodings.  A format grows through
 * feature bits: the low 32 bits are MUST-UNDERSTAND (a reader refuses a set
 * bit it does not know: the bytes would be misread without it), the high 32
 * bits are IGNORABLE (a reader that does not know one skips what it adds).
 * Representation choices a reader already decodes (tiers, widths, set vs
 * map) are content, not versions or features. */

#include <stddef.h>
#include <stdint.h>

/* Format ids: a data structure's format id is its persistType (below
 * 0x10000); containers and protocols use ids from 0x10000 */
#define DK_FORMAT_ID_PERSIST_SNAPSHOT ((uint32_t)0x10001)
#define DK_FORMAT_ID_PERSIST_WAL ((uint32_t)0x10002)
#define DK_FORMAT_ID_CLUSTER_MAILBOX ((uint32_t)0x10003)
#define DK_FORMAT_ID_FLEX_ENVELOPE ((uint32_t)0x10004)

/* Feature bits a reader must know to read the bytes at all */
#define DK_FORMAT_MUST_UNDERSTAND UINT64_C(0x00000000FFFFFFFF)

/* What a build knows about one format: the version it writes, the oldest
 * it reads, and the feature bits it understands */
typedef struct dkFormatSpec {
    const char *name; /* operator-facing name */
    uint32_t id;      /* format id (a persistType for structures) */
    uint32_t version; /* version this build writes */
    uint32_t minReadVersion;
    uint64_t knownFeatures;
} dkFormatSpec;

/* What a piece of bytes says it is */
typedef struct dkFormatStamp {
    uint32_t id;
    uint32_t version;
    uint64_t features;
} dkFormatStamp;

typedef enum dkFormatResult {
    DK_FORMAT_OK = 0,
    DK_FORMAT_MALFORMED,       /* no readable stamp */
    DK_FORMAT_WRONG_ID,        /* stamped as a different format */
    DK_FORMAT_VERSION_TOO_NEW, /* written by a newer build */
    DK_FORMAT_VERSION_TOO_OLD, /* older than this build reads */
    DK_FORMAT_FEATURE_UNKNOWN, /* a must-understand feature this build lacks */
} dkFormatResult;

/* The stamp this build writes for `spec` with `features` set */
dkFormatStamp dkFormatStampFor(const dkFormatSpec *spec, uint64_t features);

/* Whether this build reads bytes stamped `stamp` as `spec` */
dkFormatResult dkFormatCheck(const dkFormatSpec *spec,
                             const dkFormatStamp *stamp);

/* Stable name of a result, for logs and operator tooling */
const char *dkFormatResultName(dkFormatResult result);

/* ---- Standalone serializations ----------------------------------------
 *
 * A standalone serialization is a prefix flex of three unsigned entries
 * [id, version, features] followed by the payload bytes. */

/* Bytes the prefix for `stamp` takes */
size_t dkFormatPrefixSize(const dkFormatStamp *stamp);

/* Write the prefix for `stamp` into `out`; returns its size, or 0 when
 * `cap` is short */
size_t dkFormatPrefixWrite(const dkFormatStamp *stamp, uint8_t *out,
                           size_t cap);

/* Read the prefix at the start of `buf` (bounded by `len`) and check it
 * against `spec`.  On DK_FORMAT_OK, *prefixLen is the prefix's size (the
 * payload starts there).  *stamp is set whenever the prefix is readable,
 * so a refusal can report what was found. */
dkFormatResult dkFormatPrefixRead(const dkFormatSpec *spec, const uint8_t *buf,
                                  size_t len, dkFormatStamp *stamp,
                                  size_t *prefixLen);

#ifdef DATAKIT_TEST
int dkFormatTest(int argc, char *argv[]);
#endif
