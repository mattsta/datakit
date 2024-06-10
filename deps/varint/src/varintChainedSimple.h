#pragma once

__BEGIN_DECLS

/* ====================================================================
 * Chained Simple varints
 * ==================================================================== */
/* varint model Chained Simple Container:
 *   Type encoded by: reading bytes until end-of-varint marker bit found.
 *   Size: 1 byte to 9 bytes
 *   Layout: little endian
 *   Meaning: must traverse entire varint to get length.
 *            This varint uses chained "continuation bits" with a
 *            full width terminiation optimization, so storing
 *            a full 64 bit value requires 9 bytes.  Without the
 *            terminaition optimization, storing a 64 bit value
 *            would require 10 bytes with bit chaining.
 *   Pro: Three bytes can store up to 2 million.
 *        One byte can store up to 127.
 *   Con: Chained varints are slow due to zero lookahead looping. */
varintWidth varintChainedSimpleEncode64(uint8_t *p, uint64_t v);
varintWidth varintChainedSimpleLength(uint64_t v);
varintWidth varintChainedSimpleDecode64(const uint8_t *p, uint64_t *v);

varintWidth varintChainedSimpleEncode32(uint8_t *p, uint32_t v);
varintWidth varintChainedSimpleDecode32Fallback(const uint8_t *p,
                                                uint32_t *value);
varintWidth varintChainedSimpleDecode32(const uint8_t *p, uint32_t *value);

__END_DECLS
