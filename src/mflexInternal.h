#pragma once

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
};
