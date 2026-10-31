#include "mflex.h"
#include "mflexInternal.h"

#include "cleaner.h"
#include "dkDiag.h"

#define PTRLIB_BITS_LOW 2
#include "ptrlib.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(mflexState *, mflexStateFree);
defcleaner(mflex *, mflexFree);
defcleaner(flex *, flexFree);

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
/* A block this process compressed decompresses whole, or memory is corrupt:
 * the process stops (mflexOpenChecked is the form for outside bytes) */
#define _MFLEX_OPEN(m)                                                         \
    do {                                                                       \
        if (_mflexType(m) == MFLEX_TYPE_CFLEX) {                               \
            if (!cflexConvertToFlex(mfc(m), &STATE_UNCOMPRESSED_FLEX,          \
                                    &STATE_UNCOMPRESSED_SIZE)) {               \
                DK_DIAG_FATAL(DK_DIAG_MFLEX_UNREADABLE, "%s",                  \
                              "a compressed block did not decompress whole");  \
            }                                                                  \
            f = STATE_UNCOMPRESSED_FLEX;                                       \
            state->stats.decompressions++;                                     \
            STATE_UNCOMPRESSED_RETAIN;                                         \
        } else {                                                               \
            f = mff(m);                                                        \
            state->stats.plainOpens++;                                         \
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
    /* @own: caller; free=mflexStateFree */
    mflexState *state = zcalloc(1, sizeof(*state));

    size_t actualSize = jebufSizeAllocation(initialBufferSize);
    /* @own: container; free=mflexStateFree */
    STATE_UNCOMPRESSED_FLEX = zcalloc(1, actualSize);
    STATE_UNCOMPRESSED_SIZE = actualSize;

    /* @own: container; free=mflexStateFree */
    STATE_COMPRESSED_FLEX = zcalloc(1, actualSize);
    STATE_COMPRESSED_SIZE = actualSize;

    state->lenPreferred = actualSize;

    return state;
}

mflexState *mflexStateCreate(void) {
    /* @own: caller; free=mflexStateFree */
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

bool mflexStateSetPolicy(mflexState *state, uint8_t minSavingPercent,
                         cflexLevel level) {
    if (!state || minSavingPercent > MFLEX_SAVING_PERCENT_MAX ||
        level > CFLEX_LEVEL_MAX) {
        return false;
    }

    state->minSavingPercent = minSavingPercent;
    state->level = level;
    return true;
}

void mflexStateGetPolicy(const mflexState *state, uint8_t *minSavingPercent,
                         cflexLevel *level) {
    *minSavingPercent = state->minSavingPercent;
    *level = state->level;
}

void mflexStateGetStats(const mflexState *state, mflexStateStats *out) {
    *out = state->stats;
}

/* A close: 'in' bytes of flex kept as 'stored' bytes, compressed or not */
static void mflexCountClose_(mflexState *state, size_t in, size_t stored,
                             bool compressed) {
    if (compressed) {
        state->stats.closesCompressed++;
    } else {
        state->stats.closesPlain++;
    }
    state->stats.bytesIn += in;
    state->stats.bytesStored += stored;
}

/* The most bytes a compressed form of a 'totalBytes' flex may take under
 * the state's policy: strictly smaller, and saving minSavingPercent */
static size_t mflexCompressBudget_(const mflexState *state, size_t totalBytes) {
    const size_t policy =
        totalBytes - totalBytes * state->minSavingPercent / 100;
    return policy < totalBytes ? policy : totalBytes - 1;
}

size_t mflexStateBytes(const mflexState *state) {
    return sizeof(*state) + state->buf[0].len + state->buf[1].len;
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
    /* @own: caller; free=mflexFree */
    flex *f = flexNew();
    return _MFLEX_TAG(f, MFLEX_TYPE_FLEX);
}

mflex *mflexNewNoCompress(void) {
    /* @own: caller; free=mflexFree */
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
     * all the entries.
     * So, just free the original and create a new one. */
    mflexFree(*mm);

    /* @own: caller; free=mflexFree */
    *mm = mflexNew();
}

mflex *mflexDuplicate(const mflex *m) {
    const uint32_t originalType = _mflexType(m);
    size_t bytesActual = mflexBytesActual(m);

    /* @own: caller; free=mflexFree */
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
        flex *f = nullptr;                                                     \
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

void mflexPushByTypeExact(mflex **mm, mflexState *state, const databox *box,
                          flexEndpoint where) {
    MFLEX_GROW(flexPushByTypeExact, box, where);
}

void mflexBulkAppendFlex(mflex **mm, mflexState *state, const flex *elements) {
    MFLEX_GROW(flexBulkAppendFlex, elements);
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
 *
 * One mflexState supports ONE live open at a time: opening a second
 * compressed mflex through the same state decompresses over the shared
 * scratch buffer and silently invalidates the previously returned
 * flex. Use separate states for concurrent opens.
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

const flex *mflexOpenChecked(const mflex *m, mflexState *state) {
    if (_mflexType(m) != MFLEX_TYPE_CFLEX) {
        STATE_UNCOMPRESSED_RELEASE;
        state->stats.plainOpens++;
        return mff(m);
    }

    /* The header states the flex's size: LZ4 expands a byte at most 255
     * times, so a larger claim is refused before it sizes any buffer */
    const cflex *c = mfc(m);
    if (flexBytes(c) > (uint64_t)cflexBytesCompressed(c) * 255 + 64 ||
        !cflexConvertToFlex(c, &STATE_UNCOMPRESSED_FLEX,
                            &STATE_UNCOMPRESSED_SIZE) ||
        !flexValidateBufferDeep(STATE_UNCOMPRESSED_FLEX,
                                flexBytes(STATE_UNCOMPRESSED_FLEX))) {
        return nullptr;
    }

    state->stats.decompressions++;
    STATE_UNCOMPRESSED_RETAIN;
    return STATE_UNCOMPRESSED_FLEX;
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

    /* Keep the compressed form only when it is smaller than the flex by the
     * policy's saving: the scratch buffer can be bigger than 'f' (it is
     * sized up to an allocator class), so cap the budget LZ4 may fill, and
     * a flex that does not compress that far stays a flex. */
    if (flexConvertToCFlexLevel(f, STATE_COMPRESSED,
                                mflexCompressBudget_(state, totalBytes),
                                state->level)) {
        /* Now we need to return STATE_COMPRESSED, but we don't
         * want to waste 'f', so swap f<->STATE_COMPRESSED. */
        if (STATE_UNCOMPRESSED_RETAINED) {
            /* If 'f' came from STATE_UNCOMPRESSED_FLEX, then we need
             * to restore STATE_UNCOMPRESSED_FLEX with the current value
             * of 'f' (it could have been realloc'd during usage as 'f'
             * and we can't track that in STATE_UNCOMPRESSED_FLEX without
             * this manual pointer and size update here). */
            STATE_UNCOMPRESSED_FLEX = f;
            /* logical bytes UNDERSTATE the real allocation (safe: worst
             * case is a redundant realloc on a future open). Recording
             * a jebuf-class size here instead would OVERSTATE it and
             * let decompression overflow the buffer - the true
             * allocation size is not observable from a flex. */
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
        mflexCountClose_(state, totalBytes, cflexBytes(f), true);
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
        mflexCountClose_(state, totalBytes, totalBytes, false);
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
        const size_t budget = mflexCompressBudget_(state, fullSize);
        if (flexConvertToCFlexLevel(
                f, mfc(*mm), budget < mmSize ? budget : mmSize, state->level) &&
            jebufUseNewAllocation(fullSize, mflexBytesActual(*mm))) {
            /* conditions matched, so update STATE_UNCOMPRESSED_FLEX because
             * it could have been reallocated in the course of normal user
             * operations. */
            STATE_UNCOMPRESSED_FLEX = f;
            STATE_UNCOMPRESSED_SIZE = flexBytes(f);

            /* We compressed *into* '*mm', so tag it as compressed and
             * update it for the caller. */
            *mm = _MFLEX_TAG(*mm, MFLEX_TYPE_CFLEX);
            mflexCountClose_(state, fullSize, cflexBytes(mfc(*mm)), true);
        } else {
            /* else, compression failed, (meaning: this is closing after
             * deleting data, but we are _bigger_ than our original space?)
             * so don't bother compressing and just return uncompressed.
             *
             * Because 'f' was retained from STATE_UNCOMPRESSED_FLEX,
             * we're going to make the original '*mm' the new
             * STATE_UNCOMPRESSED_FLEX space now. */
            STATE_UNCOMPRESSED_FLEX = mff(*mm);
            /* The size measured before the attempt: a failed attempt has
             * already written the new flex's header into '*mm', so the
             * size it states is no longer this buffer's */
            STATE_UNCOMPRESSED_SIZE = mmSize;

            /* compression failed, so tag as not compressed and update
             * in-place for the caller. */
            *mm = _MFLEX_TAG(f, MFLEX_TYPE_FLEX);
            mflexCountClose_(state, fullSize, fullSize, false);
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
    mflexCountClose_(state, flexBytes(f), flexBytes(f), false);
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

const void *mflexStoredBytes(const mflex *m, size_t *len) {
    *len = mflexBytesActual(m);
    return mff(m);
}

mflex *mflexNewFromStored(const void *bytes, size_t len, bool compressed) {
    if (compressed ? !cflexValidateBuffer(bytes, len)
                   : !flexValidateBufferDeep(bytes, len)) {
        return nullptr;
    }

    /* @own: caller; free=mflexFree */
    void *copy = zmalloc(len);
    memcpy(copy, bytes, len);
    return _MFLEX_TAG(copy, compressed ? MFLEX_TYPE_CFLEX : MFLEX_TYPE_FLEX);
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

int mflexTest(int, char *[]) {
    int err = 0;
    int dotIncr = 0;
    static constexpr int step = 32;

    static constexpr size_t MANYDO = 8192;

    cleaner(mflexStateFree) mflexState *state = mflexStateCreate();

    TEST("create") {
        cleaner(mflexFree) mflex *m = mflexNew();
        assert(mflexBytesUncompressed(m) == FLEX_EMPTY_SIZE);
        assert(mflexBytesActual(m) == FLEX_EMPTY_SIZE);
        assert(mflexCount(m) == 0);
    }

    TEST("reset") {
        cleaner(mflexFree) mflex *m = mflexNew();
        assert(mflexBytesUncompressed(m) == FLEX_EMPTY_SIZE);
        assert(mflexBytesActual(m) == FLEX_EMPTY_SIZE);
        assert(mflexCount(m) == 0);

        mflexReset(&m);
        assert(mflexBytesUncompressed(m) == FLEX_EMPTY_SIZE);
        assert(mflexBytesActual(m) == FLEX_EMPTY_SIZE);
        assert(mflexCount(m) == 0);
    }

    TEST("duplicate empty") {
        cleaner(mflexFree) mflex *m = mflexNew();
        cleaner(mflexFree) mflex *second = mflexDuplicate(m);
        assert(mflexBytesUncompressed(second) == FLEX_EMPTY_SIZE);
        assert(mflexBytesActual(second) == FLEX_EMPTY_SIZE);
        assert(mflexCount(second) == 0);
    }

    TEST("populate entries") {
        cleaner(mflexFree) mflex *m = mflexNew();

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
    }

    mflexStateReset(state);

    TEST("remove entries") {
        cleaner(mflexFree) mflex *m = mflexNew();
        const size_t emptyBytes = mflexBytesUncompressed(m);

        for (int j = 0; j < 5; j++) {
            const size_t howMany = MANYDO;
            for (size_t i = 0; i < howMany; i++) {
                mflexPushDouble(&m, state, (double)999999999.9999999,
                                FLEX_ENDPOINT_TAIL);
                DOT(step);
            }

            DOT_CLEAR;
            if (mflexCount(m) != howMany) {
                ERR("round %d holds %zu entries, want %zu", j, mflexCount(m),
                    howMany);
            }

            size_t deleted = 0;
            while (mflexCount(m) && deleted <= howMany) {
                mflexDeleteOffsetCount(&m, state, -1, 1);
                deleted++;
                DOT(step);
            }

            DOT_CLEAR;
            if (deleted != howMany || mflexBytesUncompressed(m) != emptyBytes) {
                ERR("round %d took %zu deletes (want %zu) and left %zu "
                    "bytes (empty is %zu)",
                    j, deleted, howMany, mflexBytesUncompressed(m), emptyBytes);
            }
        }
    }

    mflexStateReset(state);

    TEST("open existing, populate more, close, open, delete each, close") {
        /* @own: container; free=mflexFree */
        flex *f = flexNew();
        for (size_t i = 0; i < MANYDO / 2; i++) {
            flexPushDouble(&f, (double)999999999.9999999, FLEX_ENDPOINT_TAIL);
            DOT(step);
        }

        DOT_CLEAR;

        cleaner(mflexFree) mflex *m = mflexConvertFromFlex(f, state);

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
    }

    mflexStateReset(state);

    TEST("open existing, remove half, close, open again") {
        /* @own: container; free=mflexFree */
        flex *f = flexNew();
        for (size_t i = 0; i < MANYDO / 2; i++) {
            flexPushDouble(&f, (double)999999999.9999999, FLEX_ENDPOINT_TAIL);
            DOT(step);
        }

        DOT_CLEAR;

        cleaner(mflexFree) mflex *m = mflexConvertFromFlex(f, state);

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
    }

    mflexStateReset(state);

    TEST("typed pushes store what the flex pushes store; preferred length") {
        cleaner(mflexStateFree) mflexState *st = mflexStateCreate();
        cleaner(mflexFree) mflex *m = mflexNew();
        cleaner(flexFree) flex *oracle = flexNew();
        if (!mflexIsEmpty(m)) {
            ERR("a new mflex is not empty");
        }
        mflexPushUnsigned(&m, st, UINT64_MAX, FLEX_ENDPOINT_TAIL);
        mflexPushFloat16(&m, st, 0.5f, FLEX_ENDPOINT_TAIL);
        mflexPushFloat(&m, st, 1.25f, FLEX_ENDPOINT_HEAD);
        flexPushUnsigned(&oracle, UINT64_MAX, FLEX_ENDPOINT_TAIL);
        flexPushFloat16(&oracle, 0.5f, FLEX_ENDPOINT_TAIL);
        flexPushFloat(&oracle, 1.25f, FLEX_ENDPOINT_HEAD);
        if (mflexIsEmpty(m) || mflexCount(m) != 3 ||
            !flexEqual(mflexOpenReadOnly(m, st), oracle)) {
            ERR("typed mflex pushes differ from the flex pushes");
        }

        /* The preferred buffer length rounds up to an allocation size */
        mflexStatePreferredLenUpdate(st, 1000);
        const size_t preferred = mflexStatePreferredLen(st);
        if (preferred < 1000 || preferred > 2048) {
            ERR("preferred length %zu for 1000", preferred);
        }
    }

    TEST("BulkAppendFlex appends what a flex append does, compressed or "
         "not") {
        /* 3000 repetitive strings (compressible) appended in batches of
         * 500 to a compressing and a never-compressing mflex: each reads
         * back as one flex holding every batch in order */
        cleaner(mflexStateFree) mflexState *st = mflexStateCreate();
        cleaner(mflexFree) mflex *compressing = mflexNew();
        cleaner(mflexFree) mflex *plain = mflexNewNoCompress();
        mflex **both[2] = {&compressing, &plain};
        cleaner(flexFree) flex *oracle = flexNew();
        for (int batch = 0; batch < 6; batch++) {
            cleaner(flexFree) flex *part = flexNew();
            for (int i = 0; i < 500; i++) {
                flexPushBytes(&part, "repeated-element-bytes", 22,
                              FLEX_ENDPOINT_TAIL);
                flexPushSigned(&part, batch * 1000 + i, FLEX_ENDPOINT_TAIL);
            }
            flexBulkAppendFlex(&oracle, part);
            for (int k = 0; k < 2; k++) {
                mflexBulkAppendFlex(both[k], st, part);
            }
        }

        for (int k = 0; k < 2; k++) {
            if (mflexCount(*both[k]) != flexCount(oracle) ||
                !flexEqual(mflexOpenReadOnly(*both[k], st), oracle)) {
                ERR("mflex %d differs from the appended flex", k);
            }
        }
        if (!mflexIsCompressed(compressing) || mflexIsCompressed(plain)) {
            ERR("compressed %d / %d", mflexIsCompressed(compressing),
                mflexIsCompressed(plain));
        }
    }

    TEST("reset then reuse: push after reset round-trips") {
        cleaner(mflexStateFree) mflexState *resetState = mflexStateCreate();
        cleaner(mflexFree) mflex *m = mflexNew();

        for (int cycle = 0; cycle < 2; cycle++) {
            for (uint64_t i = 0; i < 300; i++) {
                const databox box = databoxNewUnsigned(i);
                mflexPushByType(&m, resetState, &box, FLEX_ENDPOINT_TAIL);
            }
            if (mflexCount(m) != 300) {
                ERR("cycle %d: expected 300, got %zu", cycle, mflexCount(m));
            }

            mflexReset(&m);
            if (mflexCount(m) != 0) {
                ERR("cycle %d: expected 0 after reset, got %zu", cycle,
                    mflexCount(m));
            }
        }

        const databox box = databoxNewUnsigned(777);
        mflexPushByType(&m, resetState, &box, FLEX_ENDPOINT_TAIL);
        if (mflexCount(m) != 1) {
            ERR("Expected 1 after post-reset push, got %zu", mflexCount(m));
        }
    }

    TEST("compress policy: a block keeps its compressed form only when it "
         "saves the policy's share; LZ4HC is no larger") {
        size_t stored[3] = {};
        static const struct {
            uint8_t minSaving;
            uint8_t level;
            bool compressed;
        } runs[3] = {{0, 0, true}, {99, 0, false}, {0, 9, true}};
        for (size_t r = 0; r < countof(runs); r++) {
            cleaner(mflexStateFree) mflexState *ps = mflexStateCreate();
            if (!mflexStateSetPolicy(ps, runs[r].minSaving, runs[r].level)) {
                ERR("policy %zu refused", r);
            }
            /* @own: container; free=mflexFree */
            flex *f = flexNew();
            for (int i = 0; i < 300; i++) {
                flexPushBytes(&f, "row-of-mostly-repeating-text", 28,
                              FLEX_ENDPOINT_TAIL);
                flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
            }
            cleaner(mflexFree) mflex *m = mflexConvertFromFlex(f, ps);
            stored[r] = mflexBytesActual(m);
            if (mflexIsCompressed(m) != runs[r].compressed) {
                ERR("policy %zu: compressed %d", r, mflexIsCompressed(m));
            }
        }
        cleaner(mflexStateFree) mflexState *ps = mflexStateCreate();
        uint8_t minSaving = 1;
        cflexLevel level = 1;
        mflexStateGetPolicy(ps, &minSaving, &level);
        if (stored[2] > stored[0] || minSaving != 0 || level != 0 ||
            mflexStateSetPolicy(ps, MFLEX_SAVING_PERCENT_MAX + 1, 0) ||
            mflexStateSetPolicy(ps, 0, CFLEX_LEVEL_MAX + 1)) {
            ERR("LZ4HC %zu vs %zu bytes, or a bad policy taken", stored[2],
                stored[0]);
        }
    }

    TEST("state stats count each open and close and the bytes kept") {
        cleaner(mflexStateFree) mflexState *ss = mflexStateCreate();
        /* @own: container; free=mflexFree */
        flex *f = flexNew();
        for (int i = 0; i < 200; i++) {
            flexPushBytes(&f, "a compressible row of text", 26,
                          FLEX_ENDPOINT_TAIL);
        }
        const size_t raw = flexBytes(f);
        cleaner(mflexFree) mflex *m = mflexConvertFromFlex(f, ss);
        (void)mflexOpenReadOnly(m, ss);
        cleaner(mflexFree) mflex *plain = mflexNew();
        (void)mflexOpenReadOnly(plain, ss);
        mflexStateStats st;
        mflexStateGetStats(ss, &st);
        /* The conversion opens the flex plain, then closes it compressed */
        if (st.closesCompressed != 1 || st.closesPlain != 0 ||
            st.decompressions != 1 || st.plainOpens != 2 || st.bytesIn != raw ||
            st.bytesStored != mflexBytesActual(m)) {
            ERR("stats: %" PRIu64 " compressed closes, %" PRIu64
                " decompressions, %" PRIu64 " plain opens, %" PRIu64
                " in, %" PRIu64 " stored",
                st.closesCompressed, st.decompressions, st.plainOpens,
                st.bytesIn, st.bytesStored);
        }
    }

    TEST("a damaged compressed block: the checked open refuses it, an "
         "intact one opens to the same bytes") {
        /* @own: container; free=mflexFree */
        flex *f = flexNew();
        for (int i = 0; i < 200; i++) {
            flexPushBytes(&f, "a compressible row of text", 26,
                          FLEX_ENDPOINT_TAIL);
        }
        cleaner(flexFree) flex *want = flexDuplicate(f);
        cleaner(mflexFree) mflex *m = mflexConvertFromFlex(f, state);
        const flex *got = mflexOpenChecked(m, state);
        if (!mflexIsCompressed(m) || !got ||
            flexBytes(got) != flexBytes(want) ||
            memcmp(got, want, flexBytes(want)) != 0) {
            ERR("%s", "an intact compressed block did not open to its bytes");
        }

        /* All-zero LZ4 input names a match at offset 0: never valid */
        cflex *c = mfc(m);
        const size_t data = cflexBytesCompressed(c);
        memset(c + cflexBytes(c) - data, 0, data);
        if (mflexOpenChecked(m, state)) {
            ERR("%s", "a damaged compressed block opened");
        }
    }

    TEST("stored bytes rebuild the same block, compressed or plain; every "
         "truncation, and a compressed block taken as plain, is refused") {
        /* @own: container; free=mflexFree */
        flex *rows = flexNew();
        for (int i = 0; i < 200; i++) {
            flexPushBytes(&rows, "a compressible row of text", 26,
                          FLEX_ENDPOINT_TAIL);
        }
        /* @own: container; free=mflexFree */
        flex *few = flexNew();
        flexPushSigned(&few, 7, FLEX_ENDPOINT_TAIL);
        cleaner(mflexFree) mflex *packed = mflexConvertFromFlex(rows, state);
        cleaner(mflexFree) mflex *plain = mflexConvertFromFlex(few, state);
        const mflex *blocks[2] = {packed, plain};
        for (size_t b = 0; b < 2; b++) {
            const bool compressed = mflexIsCompressed(blocks[b]);
            size_t len;
            const uint8_t *bytes = mflexStoredBytes(blocks[b], &len);
            cleaner(mflexFree) mflex *back =
                mflexNewFromStored(bytes, len, compressed);
            size_t backLen = 0;
            const void *backBytes =
                back ? mflexStoredBytes(back, &backLen) : nullptr;
            if (compressed != (b == 0) || !back ||
                mflexIsCompressed(back) != compressed || backLen != len ||
                memcmp(backBytes, bytes, len) ||
                !mflexOpenChecked(back, state)) {
                ERR("block %zu: not rebuilt as stored", b);
            }

            for (size_t cut = 0; cut < len; cut++) {
                cleaner(mflexFree) mflex *part =
                    mflexNewFromStored(bytes, cut, compressed);
                if (part) {
                    ERR("block %zu: %zu of %zu bytes taken", b, cut, len);
                    break;
                }
            }
        }

        size_t len;
        const void *bytes = mflexStoredBytes(packed, &len);
        cleaner(mflexFree) mflex *asPlain =
            mflexNewFromStored(bytes, len, false);
        if (asPlain) {
            ERR("%s", "a compressed block taken as plain");
        }
    }

    TEST("a failed recompression after a delete that narrows the header "
         "records the scratch buffer's own size") {
        /* 200 random entries and 100 repeated ones compress (the repeats);
         * deleting the repeats leaves 200 entries (a narrower count in the
         * header) that cannot save the policy's 30%, so the recompression
         * into the old block fails part way, after writing the new
         * header.  The state then takes the old block as its scratch
         * buffer, and must record that block's size, not a size read back
         * from the half-written header */
        cleaner(mflexStateFree) mflexState *s = mflexStateCreate();
        if (!mflexStateSetPolicy(s, 30, 0)) {
            ERR("%s", "the policy was refused");
        }
        cleaner(mflexFree) mflex *m = mflexNew();
        uint64_t rng = 0x5eedc1a55ULL;
        for (int i = 0; i < 200; i++) {
            const uint64_t z = dkSplitmix64Next(&rng);
            mflexPushBytes(&m, s, &z, sizeof(z), FLEX_ENDPOINT_TAIL);
        }
        char same[200];
        memset(same, 'r', sizeof(same));
        for (int i = 0; i < 100; i++) {
            mflexPushBytes(&m, s, same, sizeof(same), FLEX_ENDPOINT_TAIL);
        }
        const size_t compressedBytes = mflexBytesActual(m);
        if (!mflexIsCompressed(m)) {
            ERR("%s", "the mixed block did not compress");
        }
        mflexDeleteOffsetCount(&m, s, 200, 100);
        if (mflexCount(m) != 200 || mflexIsCompressed(m)) {
            ERR("%zu entries left, %s", mflexCount(m),
                mflexIsCompressed(m) ? "compressed" : "plain");
        }
        if (s->buf[UNCOMPRESSED].len > compressedBytes) {
            ERR("the scratch buffer is recorded as %zu bytes; the block it "
                "took holds %zu",
                s->buf[UNCOMPRESSED].len, compressedBytes);
        }

        /* A larger compressed block opened through the same state must
         * decompress into a buffer that holds it */
        cleaner(mflexFree) mflex *big = mflexNew();
        for (int i = 0; i < 2000; i++) {
            mflexPushBytes(&big, s, same, sizeof(same), FLEX_ENDPOINT_TAIL);
        }
        const flex *open = mflexOpenReadOnly(big, s);
        if (flexCount(open) != 2000) {
            ERR("the larger block opened with %zu entries", flexCount(open));
        }
    }

    TEST("stored size never exceeds the flex, on every close path") {
        /* Incompressible (sequential and random integers, random bytes)
         * and compressible (repeated) data. After every push (grow close)
         * and every delete (shrink close), the stored size must not be
         * larger than the flex, and a compressed mflex must be strictly
         * smaller. */
        for (int pattern = 0; pattern < 4; pattern++) {
            cleaner(mflexStateFree) mflexState *s = mflexStateCreate();
            cleaner(mflexFree) mflex *m = mflexNew();
            uint64_t rng = 0x243f6a8885a308d3ULL + (uint64_t)pattern;
            for (int64_t i = 0; i < 4096; i++) {
                uint64_t z = dkSplitmix64Next(&rng);
                if (pattern == 0) {
                    mflexPushSigned(&m, s, i, FLEX_ENDPOINT_TAIL);
                } else if (pattern == 1) {
                    mflexPushSigned(&m, s, (int64_t)z, FLEX_ENDPOINT_TAIL);
                } else if (pattern == 2) {
                    mflexPushBytes(&m, s, &z, sizeof(z), FLEX_ENDPOINT_TAIL);
                } else {
                    mflexPushSigned(&m, s, 42, FLEX_ENDPOINT_TAIL);
                }

                const size_t raw = mflexBytesUncompressed(m);
                const size_t stored = mflexBytesActual(m);
                if (stored > raw || (mflexIsCompressed(m) && stored >= raw)) {
                    ERR("pattern %d push %" PRId64 ": stored %zu, flex %zu%s",
                        pattern, i, stored, raw,
                        mflexIsCompressed(m) ? " (compressed)" : "");
                    break;
                }
            }

            /* Shrink path: delete from the front until empty */
            while (mflexCount(m) > 0) {
                const size_t left = mflexCount(m);
                mflexDeleteOffsetCount(&m, s, 0,
                                       (uint32_t)(left < 37 ? left : 37));
                const size_t raw = mflexBytesUncompressed(m);
                const size_t stored = mflexBytesActual(m);
                if (stored > raw || (mflexIsCompressed(m) && stored >= raw)) {
                    ERR("pattern %d delete at %zu left: stored %zu, flex %zu",
                        pattern, mflexCount(m), stored, raw);
                    break;
                }
            }
        }
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
