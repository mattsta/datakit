#pragma once

#include "multimapCommon.h"

/* Reference Container */
#include "multimapAtom.h"

/* 8 + 4 + 4 = 16 bytes */
struct multimapSmall {
    flex *map;                              /* single map */
    uint32_t middle;                        /* offset to middle of sorted map */
    multimapElements elementsPerEntry : 16; /* max 64k "columns" per row */
    multimapElements compress : 1;    /* bool; true if compression enabled */
    multimapElements mapIsSet : 1;    /* bool; set mode dedupes WHOLE rows */
    multimapElements isSurrogate : 1; /* bool; true if all keys need refs */
    multimapElements unused : 13;     /* free flags! */
};

static_assert(sizeof(struct multimapSmall) ==
                  (sizeof(flex *) + sizeof(uint32_t) * 2),
              "multimapSmall struct is bigger than we think it should be!");

/* ====================================================================
 * Bare Small maps
 * ====================================================================
 * A Small map that is not a set and whose rows hold one or two elements
 * has no struct: its multimap pointer is its flex (ptrlibBare.h), tag 0
 * (a struct tier's are 1-3), with in its 16-bit field the size limit
 * (bits 0-3), the row width (bit 4; 0: one element, 1: two) and the
 * middle row's byte offset (bits 5-15).  A map whose middle reaches
 * MULTIMAP_BARE_MIDDLE_LIMIT keeps the struct instead.  multidict's
 * small slots are bare maps of width 2. */
#include "ptrlibBare.h"

static constexpr uint32_t MULTIMAP_BARE_MIDDLE_BITS = 11;
static constexpr uint32_t MULTIMAP_BARE_MIDDLE_LIMIT =
    1u << MULTIMAP_BARE_MIDDLE_BITS;

#define MULTIMAP_IS_BARE(m) (ptrlibBareTag(m) == 0)

DK_INLINE_ALWAYS bool multimapBareFits(const multimapElements width,
                                       const bool isSet,
                                       const uint32_t middle) {
    return !isSet && (width == 1 || width == 2) &&
           middle < MULTIMAP_BARE_MIDDLE_LIMIT;
}

DK_INLINE_ALWAYS flex *multimapBareFlex(const void *m) {
    return ptrlibBareBody(m);
}

DK_INLINE_ALWAYS uint32_t multimapBareLimit(const void *m) {
    return ptrlibBareBits(ptrlibBareField(m), 0, 4);
}

/* The pointer of a bare map: 'width' is 1 or 2, 'middle' below
 * MULTIMAP_BARE_MIDDLE_LIMIT (multimapBareFits) */
DK_INLINE_ALWAYS void *multimapBarePack(flex *map, const uint32_t middle,
                                        const multimapElements width,
                                        const uint32_t limit) {
    assert(multimapBareFits(width, false, middle));
    uint32_t field = ptrlibBareSetBits(0, 0, 4, limit);
    field = ptrlibBareSetBits(field, 4, 1, width - 1u);
    field = ptrlibBareSetBits(field, 5, MULTIMAP_BARE_MIDDLE_BITS, middle);
    return ptrlibBarePack(map, 0, field);
}

/* A bare map as a Small view on the caller's stack */
DK_INLINE_ALWAYS struct multimapSmall multimapBareView(const void *m) {
    const uint32_t field = ptrlibBareField(m);
    return (struct multimapSmall){
        .map = multimapBareFlex(m),
        .middle = ptrlibBareBits(field, 5, MULTIMAP_BARE_MIDDLE_BITS),
        .elementsPerEntry =
            (multimapElements)(1 + ptrlibBareBits(field, 4, 1))};
}
