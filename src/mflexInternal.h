#pragma once

#include "flex.h"
#include "mflex.h" /* mflexStateStats */

/* State buffer for (de)compression
 *   - buf[0] is the decompression buffer.
 *   - buf[1] is the compression buffer.
 *   - other fields are metadata. */
struct mflexState {
    struct {
        union {
            void *buf;
            flex *f;
        } ptr;
        size_t len;
        bool retained; /* 'true' if 'buf' was returned directly to a user */
    } buf[2];
    void *prevPtr;
    size_t lenPreferred;

    /* Compress policy for closes through this state: keep the compressed
     * form only when it saves at least minSavingPercent of the block, at
     * LZ4 level 'level' (0 fast, 1-12 LZ4HC) */
    uint8_t minSavingPercent;
    uint8_t level;

    /* What this state has done (mflexStateGetStats) */
    mflexStateStats stats;
};
