/* Headers have two equal sized elements, so our header size is just twice
 * our element size. */
#define _dksHeaderSize(type) (_dksHeaderElementSize(type) * 2)

DK_STATIC dksType DKS_CONTAINERTYPE(const size_t len, const size_t free) {
    if (len <= DKS_8_FULL_MAX && free <= DKS_8_SHARED_MAX) {
        return DKS_8;
    }

    if (len <= DKS_16_FULL_MAX && free <= DKS_16_SHARED_MAX) {
        return DKS_16;
    }

    if (len <= DKS_24_FULL_MAX && free <= DKS_24_SHARED_MAX) {
        return DKS_24;
    }

    if (len <= DKS_32_FULL_MAX && free <= DKS_32_SHARED_MAX) {
        return DKS_32;
    }

    if (len <= DKS_40_FULL_MAX && free <= DKS_40_SHARED_MAX) {
        return DKS_40;
    }

    if (len <= DKS_48_FULL_MAX && free <= DKS_48_SHARED_MAX) {
        return DKS_48;
    }

    assert(NULL);
    __builtin_unreachable();
}

DKS_TYPE *DKS_NEWLEN(const void *init, const size_t initlen) {
    const size_t initialFree = 0;

    const dksType type = DKS_CONTAINERTYPE(initlen, initialFree);
    const uint_fast8_t metaSize = _dksHeaderSize(type);

    /* New dks allocation with guaranteed clean memory. */
    char *dksStart =
        (char *)zcalloc(1, jebufSizeAllocation(metaSize + initlen + 1));

    /* Our dks starts after the [LENGTH][FREE_TYPE] header fields. */
    DKS_TYPE *buf = dksStart + metaSize;

    /* Initialize free space length and set type bits for this dks to 'type' */
    DKS_SETPREVIOUSINTEGERANDTYPE(buf, initialFree, type);
    assert(DKS_GETPREVIOUSINTEGERWITHTYPEREMOVED(buf, type) == initialFree);

    /* Manually set length since we have all the endpoints,
     * offsets, and math already here in this function. */
    varintExternalBigEndianPutFixedWidthQuick_((uint8_t *)dksStart, initlen,
                                               (metaSize / 2));
    assert(DKS_LEN(buf) == initlen);

    if (init && initlen) {
        memcpy(buf, init, initlen);
    }

    return buf;
}

#define DKS_LEN_SET(dks, len, type)                                            \
    do {                                                                       \
        const uint8_t headerElementSize = _dksHeaderElementSize(type);         \
        varintExternalBigEndianPutFixedWidthQuick_(                            \
            (dks) - (headerElementSize * 2), (len), headerElementSize);        \
    } while (0)

/* Common operation: update len and free then null terminate the dks. */
DK_STATIC void DKS__INFOUPDATELENFREE(dksInfo *info, const size_t len,
                                      const size_t free, const bool terminate) {
    DKS_TYPE *buf = info->buf;
    info->len = len;
    dksType type = info->type;

    DKS_LEN_SET((uint8_t *)buf, len, type);

    DKS_SETPREVIOUSINTEGERANDTYPE(buf, free, type);
    assert(DKS_TYPE_GET(info->buf) == type);
    assert(DKS_LEN(buf) == len);
    /* we could use the value of 'free' directly, but retrieving the
     * value allows for a built-in sanity check of
     * 'DKS_SETPREVIOUSINTEGERANDTYPE' working. */
    info->free = DKS_GETPREVIOUSINTEGERWITHTYPEREMOVED(buf, type);
    assert(free == info->free);

    /* termination is optional because it can require pulling in
     * memory far away from our dks header. */
    if (terminate) {
        info->buf[len] = '\0';
    }
}

/* Return pointer to where the allocation for 's' begins.
 * If 'inInfo' is not NULL, also populate 'inInfo' with metadata
 * about dks 's'. */
DK_STATIC uint8_t *DKS_BASE(const DKS_TYPE *s, dksInfo *info) {
    assert(s);
    const dksType type = DKS_TYPE_GET(s);
    const uint_fast8_t headerSize = _dksHeaderSize(type);
    uint8_t *dksStart = (uint8_t *)(s - headerSize);

    /* If we don't need to return full info, just return base pointer. */
    if (!info) {
        return dksStart;
    }

    info->type = type;
    info->start = dksStart;
    info->buf = (DKS_TYPE *)s;
    info->free = DKS_GETPREVIOUSINTEGERWITHTYPEREMOVED(s, type);
    varintExternalBigEndianGetQuick_(dksStart, headerSize / 2, info->len);

    return dksStart;
}
