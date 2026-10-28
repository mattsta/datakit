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

static_assert(sizeof(multimapSmall) == (sizeof(flex *) + sizeof(uint32_t) * 2),
              "multimapSmall struct is bigger than we think it should be!");

/* ====================================================================
 * Bare Small maps
 * ====================================================================
 * A Small map that is not a set and whose rows hold one or two elements
 * has no struct: its multimap pointer is its flex.  Bits 0-1 are 0 (a
 * struct tier's are 1-3); bits 48-51 hold the size limit, bit 52 the row
 * width (0: one element, 1: two) and bits 53-63 the middle row's byte
 * offset (the platform's pointer requirement, docs/advanced/PLATFORMS.md,
 * keeps bits 48-63 free).  A map whose middle reaches
 * MULTIMAP_BARE_MIDDLE_LIMIT keeps the struct instead.  multidict's
 * small slots are bare maps of width 2. */
static constexpr uint32_t MULTIMAP_BARE_MIDDLE_LIMIT = 1u << 11;

#define MULTIMAP_IS_BARE(m) (((uintptr_t)(m) & 3u) == 0)

DK_INLINE_ALWAYS bool multimapBareFits(const multimapElements width,
                                       const bool isSet,
                                       const uint32_t middle) {
    return !isSet && (width == 1 || width == 2) &&
           middle < MULTIMAP_BARE_MIDDLE_LIMIT;
}

DK_INLINE_ALWAYS flex *multimapBareFlex(const void *m) {
    return (flex *)((uintptr_t)m & ((UINT64_C(1) << 48) - 1));
}

DK_INLINE_ALWAYS uint32_t multimapBareLimit(const void *m) {
    return (uint32_t)(((uintptr_t)m >> 48) & 0xF);
}

/* The pointer of a bare map: 'width' is 1 or 2, 'middle' below
 * MULTIMAP_BARE_MIDDLE_LIMIT (multimapBareFits) */
DK_INLINE_ALWAYS void *multimapBarePack(flex *map, const uint32_t middle,
                                        const multimapElements width,
                                        const uint32_t limit) {
    assert(((uintptr_t)map >> 48) == 0 && ((uintptr_t)map & 3u) == 0);
    assert(multimapBareFits(width, false, middle) && limit < 16);
    return (void *)((uintptr_t)map | ((uintptr_t)limit << 48) |
                    ((uintptr_t)(width - 1) << 52) | ((uintptr_t)middle << 53));
}

/* A bare map as a Small view on the caller's stack */
DK_INLINE_ALWAYS struct multimapSmall multimapBareView(const void *m) {
    return (struct multimapSmall){
        .map = multimapBareFlex(m),
        .middle = (uint32_t)((uintptr_t)m >> 53),
        .elementsPerEntry = (multimapElements)(1 + (((uintptr_t)m >> 52) & 1))};
}
