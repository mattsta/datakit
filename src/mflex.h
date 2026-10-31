#pragma once

#include <stddef.h>
#include <stdint.h>

#include "flex.h"

/* @handle: moves: a call that can move it takes its address ('**') */
typedef struct mflex mflex;
/* @handle: fixed: its address holds from creation to free */
typedef struct mflexState mflexState;

/* State Management */
[[nodiscard]] mflexState *mflexStateNew(size_t initialBufferSize);
[[nodiscard]] mflexState *mflexStateCreate(void);
void mflexStatePreferredLenUpdate(mflexState *state, size_t len);
size_t mflexStatePreferredLen(mflexState *state);
void mflexStateReset(mflexState *state);
void mflexStateFree(mflexState *state);
/* Memory a state holds: its two scratch buffers and itself */
size_t mflexStateBytes(const mflexState *state);

/* The most a compress policy can require a block to save, in percent */
static constexpr uint8_t MFLEX_SAVING_PERCENT_MAX = 99;
/* The compress policy of closes through 'state': a block stays compressed
 * only when that saves at least minSavingPercent (0 ..
 * MFLEX_SAVING_PERCENT_MAX) of its bytes, so a block that barely compresses
 * is kept plain and opens without decompressing; at LZ4 level 'level' (0 ..
 * CFLEX_LEVEL_MAX).  A new state: 0% (any saving) at level 0.  False,
 * unchanged, for a value out of range. */
bool mflexStateSetPolicy(mflexState *state, uint8_t minSavingPercent,
                         cflexLevel level);

/* What the opens and closes through a state did: opens of compressed
 * blocks (each a decompression) and of plain ones; closes that kept the
 * block compressed and closes the policy left plain; the bytes closes took
 * in (flex) and kept (stored) */
typedef struct mflexStateStats {
    uint64_t decompressions;
    uint64_t plainOpens;
    uint64_t closesCompressed;
    uint64_t closesPlain;
    uint64_t bytesIn;
    uint64_t bytesStored;
} mflexStateStats;
void mflexStateGetStats(const mflexState *state, mflexStateStats *out);
void mflexStateGetPolicy(const mflexState *state, uint8_t *minSavingPercent,
                         cflexLevel *level);

/* mflex creation */
[[nodiscard]] mflex *mflexNew(void);
mflex *mflexNewNoCompress(void);
[[nodiscard]] mflex *mflexDuplicate(const mflex *m);
mflex *mflexConvertFromFlex(flex *f, mflexState *state);
mflex *mflexConvertFromFlexNoCompress(flex *f);

/* The bytes 'm' keeps, compressed or plain ('*len' of them, owned by 'm'):
 * with mflexIsCompressed, what mflexNewFromStored takes back */
const void *mflexStoredBytes(const mflex *m, size_t *len);
/* A block rebuilt from bytes mflexStoredBytes gave (copied), compressed
 * when 'compressed'.  The bytes may come from outside: NULL unless they
 * are a whole flex (flexValidateBufferDeep) or a whole compressed
 * envelope (cflexValidateBuffer); a compressed block's entries are
 * checked when it is first opened, with mflexOpenChecked. */
[[nodiscard]] mflex *mflexNewFromStored(const void *bytes, size_t len,
                                        bool compressed);

/* mflex metadata */
bool mflexIsEmpty(const mflex *m);
size_t mflexCount(const mflex *m);
size_t mflexBytesUncompressed(const mflex *m);
size_t mflexBytesCompressed(const mflex *m);
size_t mflexBytesActual(const mflex *m);
bool mflexIsCompressed(const mflex *m);

/* mflex clear / free */
void mflexReset(mflex **mm);
void mflexFree(mflex *m);

/* mflex push head/tail with data */
void mflexPushBytes(mflex **mm, mflexState *state, const void *s, size_t len,
                    flexEndpoint where);
void mflexPushSigned(mflex **mm, mflexState *state, const int64_t i,
                     flexEndpoint where);
void mflexPushUnsigned(mflex **mm, mflexState *state, const uint64_t u,
                       flexEndpoint where);
void mflexPushFloat16(mflex **mm, mflexState *state, const float fl,
                      flexEndpoint where);
void mflexPushFloat(mflex **mm, mflexState *state, const float fl,
                    flexEndpoint where);
void mflexPushDouble(mflex **mm, mflexState *state, const double d,
                     flexEndpoint where);
void mflexPushByType(mflex **mm, mflexState *state, const databox *box,
                     flexEndpoint where);
/* Exact path: stores `box` as given (flexPushByTypeExact); mflexPushByType
 * and mflexPushBytes auto-convert numeral bytes like flexPushByType. */
void mflexPushByTypeExact(mflex **mm, mflexState *state, const databox *box,
                          flexEndpoint where);
/* Every element of 'elements' (not consumed) appended, its bytes copied
 * as they are (flexBulkAppendFlex) */
void mflexBulkAppendFlex(mflex **mm, mflexState *state, const flex *elements);

/* mflex delete by position */
void mflexDeleteOffsetCount(mflex **mm, mflexState *state, int32_t offset,
                            uint32_t count);

/* mflex open (unwraps a flex usable for regular operations).  A block this
 * process made always opens; one that does not decompress to the size its
 * header states means memory is corrupt, and the process stops
 * (DK_DIAG_MFLEX_UNREADABLE). */
flex *mflexOpen(const mflex *m, mflexState *state);
const flex *mflexOpenReadOnly(const mflex *m, mflexState *state);
/* A read-only open for compressed blocks from outside (restored,
 * received): NULL when the stated size is past what LZ4 can expand to,
 * or the block does not decompress whole into a valid flex (entries
 * checked as flexValidateBufferDeep does).  A plain block is returned as
 * it is: its bytes are a flex the caller checked on arrival. */
const flex *mflexOpenChecked(const mflex *m, mflexState *state);

/* mflex re-compression */
void mflexCloseGrow(mflex **mm, mflexState *state, flex *f);
void mflexCloseShrink(mflex **mm, mflexState *state, flex *f);
void mflexCloseNoCompress(mflex **mm, mflexState *state, const flex *f);

/* mflex type options */
void mflexSetCompressNever(mflex **mm, mflexState *state);
void mflexSetCompressAuto(mflex **mm, mflexState *state);

#ifdef DATAKIT_TEST
int mflexTest(int argc, char *argv[]);
#endif
