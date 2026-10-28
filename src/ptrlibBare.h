#pragma once

/* A container pointer that is the container's body (a flex) with no
 * header beside it: the tier tag in bits 0-1 and a 16-bit field of the
 * container's own settings in bits 48-63.  The platform's pointer
 * requirement (docs/advanced/PLATFORMS.md: 48-bit, untagged, 16-byte
 * aligned user pointers) keeps both free; packing asserts it, so a
 * pointer that broke it fails loudly instead of reading a wrong body
 * later.  Each container names the bits of its field with
 * ptrlibBareBits / ptrlibBareSetBits; the codec is the same for all. */

#include "datakit.h"

#include <assert.h>
#include <stdint.h>

static_assert(sizeof(uintptr_t) == 8,
              "a bare container pointer keeps its field in bits 48-63");

/* The pointer of 'body' with tier 'tag' (0-3) and 'field' (16 bits) */
DK_INLINE_ALWAYS void *ptrlibBarePack(const void *body, const uint32_t tag,
                                      const uint32_t field) {
    assert(((uintptr_t)body >> 48) == 0 && ((uintptr_t)body & 3u) == 0);
    assert(tag < 4 && field <= UINT16_MAX);
    return (void *)((uintptr_t)body | tag | ((uintptr_t)field << 48));
}

/* The body a bare pointer (or any tagged pointer of this layout) points
 * to */
DK_INLINE_ALWAYS void *ptrlibBareBody(const void *p) {
    return (void *)((uintptr_t)p & ((UINT64_C(1) << 48) - 4));
}

DK_INLINE_ALWAYS uint32_t ptrlibBareTag(const void *p) {
    return (uint32_t)((uintptr_t)p & 3u);
}

DK_INLINE_ALWAYS uint32_t ptrlibBareField(const void *p) {
    return (uint32_t)((uintptr_t)p >> 48);
}

/* Bits [shift, shift + width) of a 16-bit field */
DK_INLINE_ALWAYS uint32_t ptrlibBareBits(const uint32_t field,
                                         const uint32_t shift,
                                         const uint32_t width) {
    return (field >> shift) & ((1u << width) - 1);
}

/* 'field' with bits [shift, shift + width) set to 'value' (which fits) */
DK_INLINE_ALWAYS uint32_t ptrlibBareSetBits(const uint32_t field,
                                            const uint32_t shift,
                                            const uint32_t width,
                                            const uint32_t value) {
    const uint32_t mask = ((1u << width) - 1) << shift;
    assert(value < (1u << width));
    return (field & ~mask) | (value << shift);
}
