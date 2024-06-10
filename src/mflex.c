#include "mflex.h"
#include "mflexInternal.h"

#define PTRLIB_BITS_LOW 2
#include "ptrlib.h"

typedef enum mflexType {
    MFLEX_TYPE_FLEX = 1,        /* points to 'flex *' */
    MFLEX_TYPE_CFLEX = 2,       /* points to 'cflex *' */
    MFLEX_TYPE_NO_COMPRESS = 3, /* points to 'flex *', will never compress */
} mflexType;

#define _mflexType(map) _PTRLIB_TYPE(map)
#define _MFLEX_USE(map) _PTRLIB_USE(map)
#define _MFLEX_TAG(map, type) ((mflex *)_PTRLIB_TAG(map, type))
#define _MFLEX_RETAG(map, type) ((mflex *)_PTRLIB_RETAG(map, type))

#define mff(m) ((flex *)_MFLEX_USE(m))
#define mfc(m) ((cflex *)_MFLEX_USE(m))

/* ====================================================================
 * Welcome to Macrotown
 * ==================================================================== */
enum stateBufferIndex { UNCOMPRESSED = 0, COMPRESSED = 1 };

#define _STATE_UNCOMPRESSED (state->buf[UNCOMPRESSED])
#define STATE_UNCOMPRESSED_FLEX (_STATE_UNCOMPRESSED.ptr.f)
#define STATE_UNCOMPRESSED_SIZE (_STATE_UNCOMPRESSED.len)
#define STATE_UNCOMPRESSED_RETAINED (_STATE_UNCOMPRESSED.retained)
#define STATE_UNCOMPRESSED_RETAIN (_STATE_UNCOMPRESSED.retained = true)
#define STATE_UNCOMPRESSED_RELEASE (_STATE_UNCOMPRESSED.retained = false)

#define _STATE_COMPRESSED (state->buf[COMPRESSED])
#define STATE_COMPRESSED (_STATE_COMPRESSED.ptr.buf)
#define STATE_COMPRESSED_FLEX (_STATE_COMPRESSED.ptr.f)
#define STATE_COMPRESSED_SIZE (_STATE_COMPRESSED.len)

/* ====================================================================
 * Open / Close helpers
 * ==================================================================== */
#define _MFLEX_OPEN(m)                                                         \
    do {                                                                       \
        if (_mflexType(m) == MFLEX_TYPE_CFLEX) {                               \
            /* Note: this doesn't check for decompress failure... */           \
            cflexConvertToFlex(mfc(m), &STATE_UNCOMPRESSED_FLEX,               \
                               &STATE_UNCOMPRESSED_SIZE);                      \
            f = STATE_UNCOMPRESSED_FLEX;                                       \
            STATE_UNCOMPRESSED_RETAIN;                                         \
        } else {                                                               \
            f = mff(m);                                                        \
            /* It's possible a user can OPEN then never close (for read-only   \
             * operations), so make sure to mark state as RELEASED since we    \
             * know the user isn't accessing STATE_UNCOMPRESSED_FLEX           \
             * directly. */                                                    \
            STATE_UNCOMPRESSED_RELEASE;                                        \
        }                                                                      \
    } while (0)

/* ====================================================================
 * User interface
 * ==================================================================== */
/* ====================================================================
 * State Management
 * ==================================================================== */
mflexState *mflexStateNew(size_t initialBufferSize) {
    mflexState *state = zcalloc(1, sizeof(*state));

    size_t actualSize = jebufSizeAllocation(initialBufferSize);
    STATE_UNCOMPRESSED_FLEX = zcalloc(1, actualSize);
    STATE_UNCOMPRESSED_SIZE = actualSize;

    STATE_COMPRESSED_FLEX = zcalloc(1, actualSize);
    STATE_COMPRESSED_SIZE = actualSize;

    state->lenPreferred = actualSize;

    return state;
}

mflexState *mflexStateCreate(void) {
    return mflexStateNew(65536);
}

void mflexStatePreferredLenUpdate(mflexState *state, size_t len) {
    state->lenPreferred = jebufSizeAllocation(len);
}

size_t mflexStatePreferredLen(mflexState *state) {
    return state->lenPreferred;
}

/* Grow buffers if they are currently too big or too small. */
void mflexStateReset(mflexState *state) {
    const size_t lenPreferred = state->lenPreferred;
    const size_t lenPreferredMax = lenPreferred * 2;

    /* These checks broken out individually to avoid an unnecessary loop
     * since we always have exactly 2 buffers.
     * Does it matter?  Probably not, but maybe we want to adjust the
     * UNCOMPRESSED vs. COMPRESSED buffers differently in the future? */
    const size_t len0 = STATE_UNCOMPRESSED_SIZE;
    const size_t len1 = STATE_COMPRESSED_SIZE;

    if (len0 < lenPreferred || len0 > lenPreferredMax) {
        zreallocSlateSelf(STATE_UNCOMPRESSED_FLEX, lenPreferred);
        STATE_UNCOMPRESSED_SIZE = lenPreferred;
    }

    if (len1 < lenPreferred || len1 > lenPreferredMax) {
        zreallocSlateSelf(STATE_COMPRESSED_FLEX, lenPreferred);
        STATE_COMPRESSED_SIZE = lenPreferred;
    }
}

void mflexStateFree(mflexState *state) {
    if (state) {
        zfree(STATE_UNCOMPRESSED_FLEX);
        zfree(STATE_COMPRESSED_FLEX);
        zfree(state);
    }
}

/* ====================================================================
 * mflex operations
 * ==================================================================== */
mflex *mflexNew(void) {
    flex *f = flexNew();
    return _MFLEX_TAG(f, MFLEX_TYPE_FLEX);
}

mflex *mflexNewNoCompress(void) {
    flex *f = flexNew();
    return _MFLEX_TAG(f, MFLEX_TYPE_NO_COMPRESS);
}

bool mflexIsEmpty(const mflex *m) {
    return flexCount(mff(m)) == 0;
}

size_t mflexCount(const mflex *m) {
    return flexCount(mff(m));
}

size_t mflexBytesUncompressed(const mflex *m) {
    return flexBytes(mff(m));
}

size_t mflexBytesCompressed(const mflex *m) {
    if (_mflexType(m) == MFLEX_TYPE_CFLEX) {
        return cflexBytes(mfc(m));
    }

    return 0;
}

size_t mflexBytesActual(const mflex *m) {
    if (_mflexType(m) == MFLEX_TYPE_CFLEX) {
        return cflexBytes(mfc(m));
    }

    return flexBytes(mff(m));
}

bool mflexIsCompressed(const mflex *m) {
    return _mflexType(m) == MFLEX_TYPE_CFLEX;
}

void mflexFree(mflex *m) {
    /* just drop the whole block of memory.
     * don't need to bother with any compression semantics here. */
    zfree(mff(m));
}

void mflexReset(mflex **mm) {
    /* A custom implementation of Reset() since it doesn't
     * make sense to decompress a full flex just to delete
     * all the entires.
     * So, just free the orginal and create a new one. */
    mflexFree(*mm);

    *mm = mflexNew();
}

mflex *mflexDuplicate(const mflex *m) {
    const uint32_t originalType = _mflexType(m);
    size_t bytesActual = mflexBytesActual(m);

    mflex *newMflex = zcalloc(1, bytesActual);
    memcpy(newMflex, mff(m), bytesActual);

    return _MFLEX_TAG(newMflex, originalType);
}

void mflexPushBytes(mflex **mm, mflexState *state, const void *s, size_t len,
                    flexEndpoint where) {
    flex *f;
    _MFLEX_OPEN(*mm);

    flexPushBytes(&f, s, len, where);

    mflexCloseGrow(mm, state, f);
}

#define MFLEX_GROW(fn, ...)                                                    \
    do {                                                                       \
        flex *f = NULL;                                                        \
        _MFLEX_OPEN(*mm);                                                      \
                                                                               \
        fn(&f, __VA_ARGS__);                                                   \
                                                                               \
        mflexCloseGrow(mm, state, f);                                          \
    } while (0)

#define MFLEX_SHRINK(fn, ...)                                                  \
    do {                                                                       \
        flex *f;                                                               \
        _MFLEX_OPEN(*mm);                                                      \
                                                                               \
        fn(&f, __VA_ARGS__);                                                   \
                                                                               \
        mflexCloseShrink(mm, state, f);                                        \
    } while (0)

/* ====================================================================
 * Push Endpoints
 * ==================================================================== */
/* my kingdom for a proper macro system... */
void mflexPushSigned(mflex **mm, mflexState *state, const int64_t i,
                     flexEndpoint where) {
    MFLEX_GROW(flexPushSigned, i, where);
}

void mflexPushUnsigned(mflex **mm, mflexState *state, const uint64_t u,
                       flexEndpoint where) {
    MFLEX_GROW(flexPushUnsigned, u, where);
}

void mflexPushFloat16(mflex **mm, mflexState *state, const float fl,
                      flexEndpoint where) {
    MFLEX_GROW(flexPushFloat16, fl, where);
}

void mflexPushFloat(mflex **mm, mflexState *state, const float fl,
                    flexEndpoint where) {
    MFLEX_GROW(flexPushFloat, fl, where);
}

void mflexPushDouble(mflex **mm, mflexState *state, const double d,
                     flexEndpoint where) {
    MFLEX_GROW(flexPushDouble, d, where);
}

void mflexPushByType(mflex **mm, mflexState *state, const databox *box,
                     flexEndpoint where) {
    MFLEX_GROW(flexPushByType, box, where);
}

/* ====================================================================
 * Simple deleting
 * ==================================================================== */
void mflexDeleteOffsetCount(mflex **mm, mflexState *state, int32_t offset,
                            uint32_t count) {
    MFLEX_SHRINK(flexDeleteOffsetCountDrain, offset, count);
}

/* ====================================================================
 * Open / Close for using flex* functions directly
 * ==================================================================== */
flex *mflexOpen(const mflex *m, mflexState *state) {
    flex *f;
    _MFLEX_OPEN(m);

    /* Note: this 'f' *may* or *may not* be exactly 'm'.
     *       OPEN can either return an unwrapped 'm' (if not compressed),
     *       or it can return a pointer to STATE_UNCOMPRESSED_FLEX.
     *
     *       If user modifies any data, they *must* use a close function
     *       and only retain the return value from the close function
     *       to maintain the memory integrity of both the mflex and
     *       the state buffer space.
     *
     *       Read-only operations are allowed to just use mflexOpen,
     *       use the returned flex, then never close it (since nothing
     *       changes).
     */
    return f;
}

/* if you open as ReadOnly, you never have to Close() */
const flex *mflexOpenReadOnly(const mflex *m, mflexState *state) {
    return (const flex *)mflexOpen(m, state);
}

/* Attempt to compress 'f' using STATE_COMPRESSED. */
void mflexCloseGrow(mflex **mm, mflexState *const state, flex *f) {
    if (_mflexType(*mm) == MFLEX_TYPE_NO_COMPRESS) {
        mflexCloseNoCompress(mm, state, f);
        return;
    }

    /* Lesson:
     *   This function has access to *two* or *three* memory spaces.
     *   Two memory spaces:
     *       If STATE_UNCOMPRESSED_RETAINED:
     *          'f' originated from STATE_UNCOMPRESSED_FLEX
     *       Independent memory spaces are:
     *         - '*m', 'f', and STATE_COMPRESSED.
     *         - STATE_UNCOMPRESSED_FLEX is invalid.
     *  Three memory spaces:
     *      If NOT STATE_UNCOMPRESSED_RETAINED:
     *          'f' originated from '*m'
     *      Independent memory spaces are:
     *          - 'f', STATE_UNCOMPRESSED_FLEX, STATE_COMPRESSED.
     */
    const size_t totalBytes = flexBytes(f);
    if (STATE_COMPRESSED_SIZE < totalBytes) {
        const size_t allocSize = jebufSizeAllocation(totalBytes);
        zreallocSlateSelf(STATE_COMPRESSED, allocSize);
        STATE_COMPRESSED_SIZE = allocSize;
    }

    if (flexConvertToCFlex(f, STATE_COMPRESSED, STATE_COMPRESSED_SIZE)) {
        /* Now we need to return STATE_COMPRESSED, but we don't
         * want to waste 'f', so swap f<->STATE_COMPRESSED. */
        if (STATE_UNCOMPRESSED_RETAINED) {
            /* If 'f' came from STATE_UNCOMPRESSED_FLEX, then we need
             * to restore STATE_UNCOMPRESSED_FLEX with the current value
             * of 'f' (it could have been realloc'd during usage as 'f'
             * and we can't track that in STATE_UNCOMPRESSED_FLEX without
             * this manual pointer and size update here). */
            STATE_UNCOMPRESSED_FLEX = f;
            STATE_UNCOMPRESSED_SIZE = flexBytes(f);
            STATE_UNCOMPRESSED_RELEASE;

            /* Now give the user an 'f' of the newly compressed cflex. */
            f = STATE_COMPRESSED_FLEX;

            /* Since we are returning the compressed buffer space to the user,
             * we need to disconnect the state buffer space.
             * As it happens, we can steal the user's '*mm' space as our new
             * compressed buffer space to avoid a free/malloc cycle */
            STATE_COMPRESSED_FLEX = mff(*mm);
            STATE_COMPRESSED_SIZE = mflexBytesActual(*mm);
        } else {
            /* else, swap 'f' and STATE_COMPRESSED_FLEX.
             * 'f' and STATE_UNCOMPRESSED_FLEX have always
             * been independent allocations (meaning 'f' must
             * have come from '*mm'), so return
             * STATE_COMPRESSED_FLEX to the user while saving the
             * user's 'f' (the old, currently invalid '*mm') as
             * new STATE_COMPRESSED_FLEX buffer space for future usage. */
            flex *const tmp = f;
            f = STATE_COMPRESSED_FLEX;

            STATE_COMPRESSED_FLEX = tmp;
            STATE_COMPRESSED_SIZE = flexBytes(tmp);
        }

        /* Shrink compressed flex down to the bytes it actually uses. */
        zreallocSelf(f, cflexBytes(f));
        *mm = _MFLEX_TAG(f, MFLEX_TYPE_CFLEX);
    } else {
        /* else, compression failed. */
        if (STATE_UNCOMPRESSED_RETAINED) {
            /* Here, 'f' came from STATE_UNCOMPRESSED_FLEX, so we are
             * going to return STATE_UNCOMPRESSED_FLEX to the user,
             * but the user already has '*mm' retained.
             * So, make '*mm' the new STATE_UNCOMPRESSED_FLEX. */
            STATE_UNCOMPRESSED_FLEX = mff(*mm);
            STATE_UNCOMPRESSED_SIZE = mflexBytesActual(*mm);
            STATE_UNCOMPRESSED_RELEASE;
        }

        /* This is the 'failed to compress' section, so mark
         * the current flex as uncompressed and make it the
         * new '*mm' for the caller. */
        *mm = _MFLEX_TAG(f, MFLEX_TYPE_FLEX);
    }
}

/* Attempt to compress 'f' back into original '*mm' space. */
void mflexCloseShrink(mflex **mm, mflexState *const state, flex *const f) {
    if (_mflexType(*mm) == MFLEX_TYPE_NO_COMPRESS) {
        mflexCloseNoCompress(mm, state, f);
        return;
    }

    /* This is closing *after* a delete or size reduction, so
     * attempt to re-compress back to the original mflex space. */

    if (STATE_UNCOMPRESSED_RETAINED) {
        /* 'f' came from STATE_UNCOMPRESSED_FLEX, so 'f' and '*mm' are *not*
         * the same.
         * Meaning: we can attempt to recompress 'f' back into already-existing
         *          '*mm' space. */
        const size_t mmSize = mflexBytesActual(*mm);
        const size_t fullSize = flexBytes(f);

        /* If compression worked AND compressed size is in a smaller size class,
         * return compressed version. */
        if (flexConvertToCFlex(f, mfc(*mm), mmSize) &&
            jebufUseNewAllocation(fullSize, mflexBytesActual(*mm))) {
            /* conditions matched, so update STATE_UNCOMPRESSED_FLEX because
             * it could have been reallocated in the course of normal user
             * operations. */
            STATE_UNCOMPRESSED_FLEX = f;
            STATE_UNCOMPRESSED_SIZE = flexBytes(f);

            /* We compressed *into* '*mm', so tag it as compressed and
             * update it for the caller. */
            *mm = _MFLEX_TAG(*mm, MFLEX_TYPE_CFLEX);
        } else {
            /* else, compression failed, (meaning: this is closing after
             * deleting data, but we are _bigger_ than our original space?)
             * so don't bother compressing and just return uncompressed.
             *
             * Because 'f' was retained from STATE_UNCOMPRESSED_FLEX,
             * we're going to make the original '*mm' the new
             * STATE_UNCOMPRESSED_FLEX space now. */
            STATE_UNCOMPRESSED_FLEX = mff(*mm);
            STATE_UNCOMPRESSED_SIZE = mflexBytesActual(*mm);

            /* compression failed, so tag as not compressed and update
             * in-place for the caller. */
            *mm = _MFLEX_TAG(f, MFLEX_TYPE_FLEX);
        }
    } else {
        /* else, 'f' came from '*mm' originally, so we can't optimize
         * this case.  Just run the regular 'f-vs-state' recompress. */
        mflexCloseGrow(mm, state, f);
    }
}

/* Attach 'f' back to '*mm' and perform all accounting required. */
void mflexCloseNoCompress(mflex **mm, mflexState *state, const flex *f) {
    /* If flex is the current mflex, everything is already set. */
    if (f == mff(*mm)) {
        return;
    }

    if (STATE_UNCOMPRESSED_RETAINED) {
        /* 'f' originated from STATE_UNCOMPRESSED_FLEX, so move
         * the existing '*mm' into STATE_UNCOMPRESSED_FLEX and
         * return tagged 'f' to caller. */
        STATE_UNCOMPRESSED_FLEX = mff(*mm);
        STATE_UNCOMPRESSED_SIZE = mflexBytesActual(*mm);
        STATE_UNCOMPRESSED_RELEASE;
    } /* else, 'f' originated from '*mm', so just re-tag it directly. */

    /* in all cases we want to directly replace '*mm' with 'f' itself. */
    *mm = _MFLEX_TAG(f, MFLEX_TYPE_NO_COMPRESS);
}

/* ====================================================================
 * Re-tag mflex as never compress
 * ==================================================================== */
void mflexSetCompressNever(mflex **mm, mflexState *state) {
    const uint32_t type = _mflexType(*mm);
    if (type == MFLEX_TYPE_NO_COMPRESS) {
        return;
    }

    if (type == MFLEX_TYPE_FLEX) {
        *mm = _MFLEX_RETAG(*mm, MFLEX_TYPE_NO_COMPRESS);
        return;
    }

    flex *f;
    _MFLEX_OPEN(*mm);

    mflexCloseNoCompress(mm, state, f);
    *mm = _MFLEX_RETAG(*mm, MFLEX_TYPE_NO_COMPRESS);
}

void mflexSetCompressAuto(mflex **mm, mflexState *state) {
    /* If already compressed, don't change anything. */
    if (_mflexType(*mm) == MFLEX_TYPE_CFLEX) {
        return;
    }

    flex *f;

    *mm = _MFLEX_RETAG(*mm, MFLEX_TYPE_FLEX);
    _MFLEX_OPEN(*mm);

    mflexCloseGrow(mm, state, f);
}

mflex *mflexConvertFromFlex(flex *f, mflexState *state) {
    mflex *m = (mflex *)f;
    mflexSetCompressAuto(&m, state);
    return m;
}

mflex *mflexConvertFromFlexNoCompress(flex *f) {
    mflex *m = _MFLEX_TAG(f, MFLEX_TYPE_NO_COMPRESS);
    return m;
}

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST

#include "ctest.h"

#define DOT(step)                                                              \
    do {                                                                       \
        if (dotIncr++ % (step) == 0) {                                         \
            fwrite(".", 1, 1, stdout);                                         \
            fflush(stdout);                                                    \
        }                                                                      \
    } while (0)

#define DOT_CLEAR                                                              \
    do {                                                                       \
        dotIncr = 0;                                                           \
        fwrite("\n", 1, 1, stdout);                                            \
        fflush(stdout);                                                        \
    } while (0)

int mflexTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    int dotIncr = 0;
    static const int step = 32;

    static const size_t MANYDO = 8192;

    mflexState *state = mflexStateCreate();

    TEST("create") {
        mflex *m = mflexNew();
        assert(mflexBytesUncompressed(m) == FLEX_EMPTY_SIZE);
        assert(mflexBytesActual(m) == FLEX_EMPTY_SIZE);
        assert(mflexCount(m) == 0);
        mflexFree(m);
    }

    TEST("reset") {
        mflex *m = mflexNew();
        assert(mflexBytesUncompressed(m) == FLEX_EMPTY_SIZE);
        assert(mflexBytesActual(m) == FLEX_EMPTY_SIZE);
        assert(mflexCount(m) == 0);

        mflexReset(&m);
        assert(mflexBytesUncompressed(m) == FLEX_EMPTY_SIZE);
        assert(mflexBytesActual(m) == FLEX_EMPTY_SIZE);
        assert(mflexCount(m) == 0);

        mflexFree(m);
    }

    TEST("duplicate empty") {
        mflex *m = mflexNew();
        mflex *second = mflexDuplicate(m);
        assert(mflexBytesUncompressed(second) == FLEX_EMPTY_SIZE);
        assert(mflexBytesActual(second) == FLEX_EMPTY_SIZE);
        assert(mflexCount(second) == 0);

        mflexFree(second);
        mflexFree(m);
    }

    TEST("populate entries") {
        mflex *m = mflexNew();

        const size_t howMany = MANYDO;
        for (size_t i = 0; i < howMany; i++) {
            mflexPushDouble(&m, state, (double)999999999.9999999,
                            FLEX_ENDPOINT_TAIL);
            assert(!STATE_UNCOMPRESSED_RETAINED);
            DOT(step);
        }

        DOT_CLEAR;

        printf("Size uncompressed: %zu\n", mflexBytesUncompressed(m));
        printf("Size compressed: %zu\n", mflexBytesCompressed(m));
        printf("Size current: %zu\n", mflexBytesActual(m));

        assert(mflexCount(m) == howMany);
        assert(mflexBytesActual(m) != mflexBytesUncompressed(m));

        mflexFree(m);
    }

    mflexStateReset(state);

    TEST("remove entries") {
        mflex *m = mflexNew();

        for (int j = 0; j < 5; j++) {
            const size_t howMany = MANYDO;
            for (size_t i = 0; i < howMany; i++) {
                mflexPushDouble(&m, state, (double)999999999.9999999,
                                FLEX_ENDPOINT_TAIL);
                DOT(step);
            }

            DOT_CLEAR;

            while (mflexCount(m)) {
                mflexDeleteOffsetCount(&m, state, -1, 1);
                DOT(step);
            }

            DOT_CLEAR;
        }

        mflexFree(m);
    }

    mflexStateReset(state);

    TEST("open existing, populate more, close, open, delete each, close") {
        flex *f = flexNew();
        for (size_t i = 0; i < MANYDO / 2; i++) {
            flexPushDouble(&f, (double)999999999.9999999, FLEX_ENDPOINT_TAIL);
            DOT(step);
        }

        DOT_CLEAR;

        mflex *m = mflexConvertFromFlex(f, state);

        /* verify is compressed */
        assert(mflexBytesActual(m) != mflexBytesUncompressed(m));

        /* open */
        f = mflexOpen(m, state);

        /* add more */
        for (size_t i = 0; i < MANYDO / 2; i++) {
            flexPushDouble(&f, (double)999999999.9999999, FLEX_ENDPOINT_TAIL);
            DOT(step);
        }

        DOT_CLEAR;

        /* close */
        mflexCloseGrow(&m, state, f);

        /* verify is still compressed */
        assert(mflexBytesActual(m) != mflexBytesUncompressed(m));

        /* open */
        f = mflexOpen(m, state);

        /* delete all */
        while (flexCount(f)) {
            flexDeleteOffsetCountDrain(&f, -1, 1);
            DOT(step);
        }

        DOT_CLEAR;

        /* close */
        mflexCloseShrink(&m, state, f);

        /* verify none remain */
        assert(mflexBytesActual(m) == FLEX_EMPTY_SIZE);
        assert(mflexCount(m) == 0);

        mflexFree(m);
    }

    mflexStateReset(state);

    TEST("open existing, remove half, close, open again") {
        flex *f = flexNew();
        for (size_t i = 0; i < MANYDO / 2; i++) {
            flexPushDouble(&f, (double)999999999.9999999, FLEX_ENDPOINT_TAIL);
            DOT(step);
        }

        DOT_CLEAR;

        mflex *m = mflexConvertFromFlex(f, state);

        /* verify is compressed */
        assert(mflexBytesActual(m) != mflexBytesUncompressed(m));

        /* open */
        f = mflexOpen(m, state);

        /* delete half */
        while (flexCount(f) > MANYDO / 4) {
            flexDeleteOffsetCountDrain(&f, -1, 1);
            DOT(step);
        }

        DOT_CLEAR;

        /* close */
        mflexCloseShrink(&m, state, f);

        /* verify none remain */
        assert(mflexBytesActual(m) > FLEX_EMPTY_SIZE);
        assert(mflexCount(m) == MANYDO / 4);

        mflexFree(m);
    }

    mflexStateReset(state);
    mflexStateFree(state);

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
