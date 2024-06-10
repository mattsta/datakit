/* Headers have one element: the string length. */
#define _dksHeaderSize(type) (_dksHeaderElementSize(type))

DK_STATIC dksType DKS_CONTAINERTYPE(const size_t len, const size_t free) {
    (void)free;
    if (len <= DKS_8_SHARED_MAX) {
        return DKS_8;
    }

    if (len <= DKS_16_SHARED_MAX) {
        return DKS_16;
    }

    if (len <= DKS_24_SHARED_MAX) {
        return DKS_24;
    }

    if (len <= DKS_32_SHARED_MAX) {
        return DKS_32;
    }

    if (len <= DKS_40_SHARED_MAX) {
        return DKS_40;
    }

    if (len <= DKS_48_SHARED_MAX) {
        return DKS_48;
    }

    assert(NULL);
    __builtin_unreachable();
}

DKS_TYPE *DKS_NEWLEN(const void *init, const size_t initlen) {
    const dksType type = DKS_CONTAINERTYPE(initlen, 0);
    const uint_fast8_t metaSize = _dksHeaderSize(type);

    /* New dks allocation with guaranteed clean memory. */
    char *dksStart = (char *)zcalloc(1, metaSize + initlen + 1);

    /* Our dks starts after the [LENGTH][FREE_TYPE] header fields. */
    DKS_TYPE *buf = dksStart + metaSize;

    /* Initialize free space length and set type bits for this dks to 'type' */
    DKS_SETPREVIOUSINTEGERANDTYPE(buf, initlen, type);
    assert(DKS_GETPREVIOUSINTEGERWITHTYPEREMOVED(buf, type) == initlen);

    assert(DKS_LEN(buf) == initlen);

    if (init && initlen) {
        memcpy(buf, init, initlen);
    }

    return buf;
}

#define DKS_LEN_SET(dks, len, type)                                            \
    (DKS_SETPREVIOUSINTEGERANDTYPE(buf, len, type))

/* Common operation: update len and free then null terminate the dks. */
DK_STATIC void DKS__INFOUPDATELENFREE(dksInfo *info, const size_t len,
                                      const size_t free, const bool terminate) {
    (void)free;
    DKS_TYPE *buf = info->buf;
    info->len = len;
    dksType type = info->type;

    DKS_LEN_SET((uint8_t *)buf, len, type);

    assert(DKS_TYPE_GET(info->buf) == type);
    assert(DKS_LEN(buf) == len);
    /* here 'free' is always 0 because COMPACT has no free size ever.
     * it is 'compact' because it is only [LENGTH][DATA] not
     * [LENGTH][FREE][DATA] */
    info->free = 0;

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
    info->len = DKS_GETPREVIOUSINTEGERWITHTYPEREMOVED(s, type);
    info->free = 0; /* no 'free' because this is _COMPACT_ */

    return dksStart;
}
