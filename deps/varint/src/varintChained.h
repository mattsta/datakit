#pragma once

#include "varint.h"
__BEGIN_DECLS

/*******************************************************************************
 * Legacy testing for benchmark comparisons
 ******************************************************************************/
/* varint model Chained Container:
 *   Type encoded by: reading bytes until end-of-varint marker bit is found.
 *   Size: 1 byte to 9 bytes
 *   Meaning: must traverse entire varint to get length.
 *            This varint uses chained "continuation bits" to mark end of the
 *            varint.  Since one bit is reserved for "continuation," each byte
 *            has 7 usable bits unless a full 9 bytes are used.  If a full 9
 *            bytes are used, this encoding has an optimization to *not* store
 *            a continuation bit in the final byte, so we save one byte
 *            by being able to use the full 8 bits in the last byte (if we
 *            are using the maximum 9-byte width).
 *   Pro: Three bytes can store up to 2 million.
 *        One byte can store up to 127.
 *   Con: Chained varints are slower than every other varint type due to
 *        continuation bit chaining. */
varintWidth varintChainedPutVarint(uint8_t *p, uint64_t v);
varintWidth varintChainedGetVarint(const uint8_t *p, uint64_t *v);
varintWidth varintChainedGetVarint32(const uint8_t *p, uint32_t *v);
varintWidth varintChainedVarintLen(uint64_t v);
varintWidth varintChainedVarintAddNoGrow(uint8_t *z, int64_t add);
varintWidth varintChainedVarintAddGrow(uint8_t *z, int64_t add);

/*
** The common case is for a varint to be a single byte.  They following
** macros handle the common case without a procedure call, but then call
** the procedure for larger varints.
*/
#define varintChained_getVarint32(A, B)                                        \
    (uint8_t)((*(A) < (uint8_t)0x80) ? ((B) = (uint32_t) * (A)),               \
              1 : varintChainedGetVarint32((A), (uint32_t *)&(B)))
#define varintChained_putVarint32(A, B)                                        \
    (uint8_t)(((uint32_t)(B) < (uint32_t)0x80) ? (*(A) = (unsigned char)(B)),  \
              1 : varintChainedPutVarint((A), (B)))

__END_DECLS
