/**
 * ptrlib core contract tests
 *
 * ptrlib is the pointer-tagging foundation under multilist, mflex,
 * multimap tiers, intset tiers, and the compress-metadata top bits.
 * These tests pin the bit-level contracts directly so failures point
 * at the foundation instead of whichever structure is stacked on top.
 *
 * Configured with the most common consumer shape: 2 low type bits plus
 * the full 16 high metadata bits (the multilist/mflex configuration).
 */
#include "datakit.h"

#define PTRLIB_BITS_LOW 2
#define PTRLIB_BITS_HIGH 16
#include "ptrlib.h"

#include "ptrlibCompress.h"

#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(void *, zfree);

int ptrlibTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    /* A realistic 16-byte-aligned heap pointer for round-trips */
    cleaner(zfree) void *alloc = zcalloc(1, 64);
    const uintptr_t base = (uintptr_t)alloc;

    TEST("GENMASK widths") {
        if (_PTRLIB_GENMASK(1) != 0x1ULL || _PTRLIB_GENMASK(2) != 0x3ULL ||
            _PTRLIB_GENMASK(8) != 0xffULL || _PTRLIB_GENMASK(16) != 0xffffULL) {
            ERRR("GENMASK produced wrong masks");
        }
    }

    TEST("low bits: tag/type/use/retag round-trips") {
        for (uintptr_t type = 0; type <= _PTRLIB_TAGGED_PTR_MASK; type++) {
            void *tagged = (void *)_PTRLIB_TAG(alloc, type);
            if (_PTRLIB_TYPE(tagged) != type) {
                ERR("TYPE lost for tag %zu", (size_t)type);
            }
            if ((uintptr_t)_PTRLIB_USE(tagged) != base) {
                ERR("USE did not restore base for tag %zu", (size_t)type);
            }

            const uintptr_t other = (type + 1) & _PTRLIB_TAGGED_PTR_MASK;
            void *retagged = (void *)_PTRLIB_RETAG(tagged, other);
            if (_PTRLIB_TYPE(retagged) != other ||
                (uintptr_t)_PTRLIB_USE(retagged) != base) {
                ERR("RETAG corrupted pointer for %zu->%zu", (size_t)type,
                    (size_t)other);
            }
        }
    }

    TEST("top bits: 8_1/8_2/16 set+get round-trips at boundaries") {
        const uintptr_t vals8[] = {0, 1, 0x7f, 0x80, 0xff};
        for (size_t i = 0; i < sizeof(vals8) / sizeof(vals8[0]); i++) {
            void *p1 = _PTRLIB_TOP_SET_8_1(alloc, vals8[i]);
            if (_PTRLIB_TOP_8_1(p1) != vals8[i]) {
                ERR("8_1 round-trip failed for 0x%zx", (size_t)vals8[i]);
            }
            if (_PTRLIB_TOP_8_2(p1) != 0) {
                ERR("8_1 set leaked into 8_2 for 0x%zx", (size_t)vals8[i]);
            }

            void *p2 = _PTRLIB_TOP_SET_8_2(alloc, vals8[i]);
            if (_PTRLIB_TOP_8_2(p2) != vals8[i]) {
                ERR("8_2 round-trip failed for 0x%zx", (size_t)vals8[i]);
            }
            if (_PTRLIB_TOP_8_1(p2) != 0) {
                ERR("8_2 set leaked into 8_1 for 0x%zx", (size_t)vals8[i]);
            }
        }

        void *p16 = _PTRLIB_TOP_SET_16(alloc, 0xBEEF);
        if (_PTRLIB_TOP_16(p16) != 0xBEEF) {
            ERRR("16-bit round-trip failed");
        }
        /* Layout fact: the 16-bit field spans bits 48-63; 8_1 is its
         * LOW byte (bits 48-55) and 8_2 its HIGH byte (bits 56-63) */
        if (_PTRLIB_TOP_8_1(p16) != 0xEF || _PTRLIB_TOP_8_2(p16) != 0xBE) {
            ERRR("16-bit field does not decompose into 8_1(low)|8_2(high)");
        }
    }

    TEST("top bits: SET is OR-only; RESET replaces") {
        /* SET on a dirty field ORs (documented sharp edge); RESET is
         * the clear+set that field updates must use */
        void *dirty = _PTRLIB_TOP_SET_8_1(alloc, 0xF0);
        void *orred = _PTRLIB_TOP_SET_8_1(dirty, 0x0F);
        if (_PTRLIB_TOP_8_1(orred) != 0xFF) {
            ERRR("SET_8_1 is expected to OR into the field");
        }

        void *reset = _PTRLIB_TOP_RESET_8_1(dirty, 0x0F);
        if (_PTRLIB_TOP_8_1(reset) != 0x0F) {
            ERRR("RESET_8_1 must replace the field");
        }

        void *reset2 =
            _PTRLIB_TOP_RESET_8_2(_PTRLIB_TOP_SET_8_2(alloc, 0xAA), 0x55);
        if (_PTRLIB_TOP_8_2(reset2) != 0x55) {
            ERRR("RESET_8_2 must replace the field");
        }
    }

    TEST("top bits: BOOL_N set/get/reset for every n") {
        for (int n = 1; n <= 16; n++) {
            void *on = _PTRLIB_TOP_SET_BOOL_N(alloc, n, 1);
            if (_PTRLIB_TOP_BOOL_N(on, n) != 1) {
                ERR("BOOL_%d set/get failed", n);
            }

            void *off = _PTRLIB_TOP_RESET_BOOL_N(on, n, 0);
            if (_PTRLIB_TOP_BOOL_N(off, n) != 0) {
                ERR("BOOL_%d reset-to-0 failed", n);
            }

            /* neighboring bools must be untouched */
            if (n > 1 && _PTRLIB_TOP_BOOL_N(on, n - 1) != 0) {
                ERR("BOOL_%d set leaked into BOOL_%d", n, n - 1);
            }
        }
    }

    TEST("interaction: low tag preserves top bits and vice versa") {
        /* The contract the multilist Medium promotion violated: every
         * combining order must preserve the other layer's bits */
        void *meta =
            _PTRLIB_TOP_SET_8_2(_PTRLIB_TOP_SET_8_1(alloc, 0x21), 0x43);
        void *both = (void *)_PTRLIB_TAG(meta, 2);

        if (_PTRLIB_TYPE(both) != 2) {
            ERRR("TAG after top-set lost the type");
        }
        if (_PTRLIB_TOP_8_1(both) != 0x21 || _PTRLIB_TOP_8_2(both) != 0x43) {
            ERRR("TAG destroyed top metadata");
        }

        /* opposite order */
        void *tagFirst = (void *)_PTRLIB_TAG(alloc, 3);
        void *metaAfter = _PTRLIB_TOP_SET_8_1(tagFirst, 0x66);
        if (_PTRLIB_TYPE(metaAfter) != 3 ||
            _PTRLIB_TOP_8_1(metaAfter) != 0x66) {
            ERRR("top-set destroyed the low tag");
        }

        /* USE strips low only; TOP_USE strips top only; USE_ALL both */
        if ((uintptr_t)_PTRLIB_USE(both) == base) { /* still carries top bits */
            ERRR("USE unexpectedly stripped top bits");
        }
        if (_PTRLIB_TYPE(_PTRLIB_TOP_USE(both)) != 2) {
            ERRR("TOP_USE unexpectedly stripped the low tag");
        }
        if ((uintptr_t)_PTRLIB_TOP_USE_ALL(both) != base) {
            ERRR("TOP_USE_ALL did not restore the raw pointer");
        }
    }

    TEST("interaction: RESET touches only its own field") {
        void *full = (void *)_PTRLIB_TAG(
            _PTRLIB_TOP_SET_8_2(_PTRLIB_TOP_SET_8_1(alloc, 0xAB), 0xCD), 1);

        void *r1 = _PTRLIB_TOP_RESET_8_1(full, 0x11);
        if (_PTRLIB_TOP_8_1(r1) != 0x11 || _PTRLIB_TOP_8_2(r1) != 0xCD ||
            _PTRLIB_TYPE(r1) != 1 ||
            (uintptr_t)_PTRLIB_TOP_USE_ALL(r1) != base) {
            ERRR("RESET_8_1 disturbed a neighboring layer");
        }

        void *r2 = _PTRLIB_TOP_RESET_8_2(full, 0x22);
        if (_PTRLIB_TOP_8_2(r2) != 0x22 || _PTRLIB_TOP_8_1(r2) != 0xAB ||
            _PTRLIB_TYPE(r2) != 1) {
            ERRR("RESET_8_2 disturbed a neighboring layer");
        }
    }

    TEST("compress facade: depth/limit round-trip with type tag") {
        /* ptrlibCompress.h is the production consumer of 8_1/8_2 */
        void *p = SET_COMPRESS_DEPTH_LIMIT(alloc, 7, 6);
        p = (void *)_PTRLIB_TAG(p, 2);

        if (COMPRESS_DEPTH(p) != 7 || COMPRESS_LIMIT(p) != 6) {
            ERRR("depth/limit lost after tagging");
        }
        if (_PTRLIB_TYPE(p) != 2) {
            ERRR("type lost after depth/limit set");
        }

        p = RESET_COMPRESS_LIMIT(p, 9);
        if (COMPRESS_LIMIT(p) != 9 || COMPRESS_DEPTH(p) != 7 ||
            _PTRLIB_TYPE(p) != 2) {
            ERRR("RESET_COMPRESS_LIMIT disturbed other fields");
        }

        if ((uintptr_t)REMOVE_META(p) != (uintptr_t)_PTRLIB_TAG(alloc, 2)) {
            ERRR("REMOVE_META must strip top bits and keep the low tag");
        }
    }

    TEST("round-trip through memory: tagged pointer still dereferences") {
        void *loaded = (void *)_PTRLIB_TAG(
            _PTRLIB_TOP_SET_8_2(_PTRLIB_TOP_SET_8_1(alloc, 0x5A), 0xA5), 3);
        uint8_t *raw = (uint8_t *)_PTRLIB_TOP_USE_ALL(loaded);
        raw[0] = 0x77;
        raw[63] = 0x99;
        if (((uint8_t *)alloc)[0] != 0x77 || ((uint8_t *)alloc)[63] != 0x99) {
            ERRR("untagged pointer does not address the allocation");
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
