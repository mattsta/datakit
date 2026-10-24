#include "multiarraySmall.h"
#include "multiarraySmallInternal.h"

#include "dkDiag.h"

void *multiarrayIndexRefused_(const char *function, const char *file, int line,
                              int64_t idx, uint64_t count) {
    dkDiagReport_(DK_DIAG_MISUSE, DK_DIAG_MULTIARRAY_INDEX, function, file,
                  line,
                  "index %" PRId64 " (range %" PRIu64 ") is outside the array",
                  idx, count ? count : 1);
    return NULL;
}

multiarraySmall *multiarraySmallNew(uint16_t len, uint16_t rowMax) {
    /* @own: caller; free=multiarraySmallFree */
    multiarraySmall *mar = zcalloc(1, sizeof(*mar));
    mar->len = len;
    mar->rowMax = rowMax;
    /* @own: container; free=multiarraySmallFree */
    mar->data = zcalloc(1, len);
    return mar;
}

void multiarraySmallFree(multiarraySmall *mar) {
    if (mar) {
        zfree(mar->data);
        zfree(mar);
    }
}

/* multiarray offset helper macro (size_t math: uint16*uint16 products
 * would promote to int and can overflow at extreme len*count) */
#define _mo(offset) ((size_t)mar->len * (offset))
void multiarraySmallInsert(multiarraySmall *mar, uint32_t idx, void *s) {
    /* idx may be at most one past the end (the append position) */
    if (unlikely(idx > mar->count)) {
        MA_INDEX_REFUSED_(idx);
        return;
    }

    /* @own: container; free=multiarraySmallFree */
    mar->data = zrealloc(mar->data, _mo(mar->count + 1));

    if (idx < mar->count) {
        const uint32_t remaining = mar->count - idx;
        memmove(mar->data + _mo(idx + 1), mar->data + _mo(idx), _mo(remaining));
    }

    memcpy(mar->data + _mo(idx), s, mar->len);
    mar->count++;
}

size_t multiarraySmallCount(const multiarraySmall *mar) {
    return mar->count;
}

size_t multiarraySmallBytes(const multiarraySmall *mar) {
    /* Bytes of element data storage (metadata overhead excluded) */
    return (size_t)mar->len * mar->count;
}

void multiarraySmallDelete(multiarraySmall *mar, uint32_t idx) {
    multiarraySmallDeleteTaking(mar, idx, NULL);
}

/* Delete entry 'idx', first copying it to 'out' when 'out' is not NULL */
void multiarraySmallDeleteTaking(multiarraySmall *mar, uint32_t idx,
                                 void *out) {
    /* Past the end (any index of an empty array): refused, 'out'
     * untouched */
    if (unlikely(idx >= mar->count)) {
        MA_INDEX_REFUSED_(idx);
        return;
    }

    if (out) {
        memcpy(out, mar->data + _mo(idx), mar->len);
    }

    const uint32_t remaining = mar->count - idx - 1;
    memmove(mar->data + _mo(idx), mar->data + _mo(idx + 1), _mo(remaining));

    /* @own: container; free=multiarraySmallFree */
    mar->data = zrealloc(mar->data, _mo(mar->count - 1));
    mar->count--;
}

/* Delete the 'count' entries from 'idx' (all within the array) with one
 * move and one resize */
void multiarraySmallDeleteRange(multiarraySmall *mar, uint32_t idx,
                                uint32_t count) {
    /* An empty range changes nothing, wherever it is (as in every tier) */
    if (count == 0) {
        return;
    }
    if (unlikely((uint64_t)idx + count > mar->count)) {
        MA_RANGE_REFUSED_(idx, count);
        return;
    }

    const uint32_t after = (uint32_t)mar->count - idx - count;
    memmove(mar->data + _mo(idx), mar->data + _mo((uint32_t)idx + count),
            _mo(after));

    /* @own: container; free=multiarraySmallFree */
    mar->data = zrealloc(mar->data, _mo(mar->count - count));
    mar->count -= count;
}

#ifdef DATAKIT_TEST

#include "ctest.h"

#include <assert.h>

typedef struct s16 {
    int64_t a;
    int64_t b;
} s16;

int multiarraySmallTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    printf("Testing direct...\n");
    static const int globalMax = 2048;
    TEST("create") {
        s16 *s = multiarraySmallDirectNew(s);
        assert(s);

        int count = 0;
        int idx = 0;
        multiarraySmallDirectInsert(s, count, idx);

        multiarraySmallDirectFree(s);
    }

    TEST("insert before") {
        s16 *s = multiarraySmallDirectNew(s);
        assert(s);

        int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            multiarraySmallDirectInsert(s, count, idx);
            s[idx].a = idx;
            s[idx].b = idx;
            count++;
        }

        for (int idx = 0; idx < globalMax; idx++) {
            assert(s[idx].a == idx);
            assert(s[idx].b == idx);
        }

        multiarraySmallDirectFree(s);
    }

    TEST("insert before constant zero") {
        s16 *s = multiarraySmallDirectNew(s);
        assert(s);

        int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            multiarraySmallDirectInsert(s, count, 0);
            s[0].a = idx;
            s[0].b = idx;
            count++;
        }

        for (int idx = 0; idx < globalMax; idx++) {
            assert(s[idx].a == globalMax - 1 - idx);
            assert(s[idx].b == globalMax - 1 - idx);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            multiarraySmallDirectDelete(s, count, 0);
            count--;
        }

        multiarraySmallDirectFree(s);
    }

    TEST("insert after") {
        s16 *s = multiarraySmallDirectNew(s);
        assert(s);

        int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            multiarraySmallDirectInsert(s, count + 1, idx + 1);
            s[idx + 1].a = idx;
            s[idx + 1].b = idx;
            count++;
        }

        for (int idx = 0; idx < globalMax; idx++) {
            assert(s[idx + 1].a == idx);
            assert(s[idx + 1].b == idx);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            /* Delete reverse */
            multiarraySmallDirectDelete(s, count, globalMax - idx - 1);
            count--;
        }

        multiarraySmallDirectFree(s);
    }

    printf("Testing container...\n");
    const int16_t rowMax = 0; /* not used for testing single smalls */
    TEST("create") {
        multiarraySmall *s = multiarraySmallNew(sizeof(s16), rowMax);
        assert(s);

        int idx = 0;
        s16 _s = {0};
        multiarraySmallInsert(s, idx, &_s);

        multiarraySmallFree(s);
    }

    TEST("insert before") {
        multiarraySmall *s = multiarraySmallNew(sizeof(s16), rowMax);
        assert(s);

        int count = 0;
        s16 _s = {0};
        for (int idx = 0; idx < globalMax; idx++) {
            _s.a = idx;
            _s.b = idx;
            multiarraySmallInsert(s, idx, &_s);
            count++;
        }

        assert(count == s->count);

        for (int idx = 0; idx < globalMax; idx++) {
            assert(((s16 *)multiarraySmallGet(s, idx))->a == idx);
            assert(((s16 *)multiarraySmallGet(s, idx))->b == idx);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            multiarraySmallDelete(s, 0);
        }

        multiarraySmallFree(s);
    }

    TEST("insert before constant zero") {
        multiarraySmall *s = multiarraySmallNew(sizeof(s16), rowMax);
        assert(s);

        int count = 0;
        s16 _s = {0};
        for (int idx = 0; idx < globalMax; idx++) {
            _s.a = idx;
            _s.b = idx;
            multiarraySmallInsert(s, 0, &_s);
            count++;
        }

        assert(count == s->count);

        for (int idx = 0; idx < globalMax; idx++) {
            assert(((s16 *)multiarraySmallGet(s, idx))->a ==
                   globalMax - 1 - idx);
            assert(((s16 *)multiarraySmallGet(s, idx))->b ==
                   globalMax - 1 - idx);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            multiarraySmallDelete(s, 0);
        }

        multiarraySmallFree(s);
    }

    TEST("insert after") {
        multiarraySmall *s = multiarraySmallNew(sizeof(s16), rowMax);
        assert(s);

        int count = 0;
        s16 _s = {0};

        /* Because we are inserting *after* we need to fake our
         * initial count so we have +1 added everywhere for the
         * correct 'after' entry to exist. */
        s->count = 1;
        for (int idx = 0; idx < globalMax; idx++) {
            _s.a = idx;
            _s.b = idx;
            multiarraySmallInsert(s, idx + 1, &_s);
            count++;
        }

        assert(count + 1 == s->count);

        for (int idx = 0; idx < globalMax; idx++) {
            assert(((s16 *)multiarraySmallGet(s, idx + 1))->a == idx);
            assert(((s16 *)multiarraySmallGet(s, idx + 1))->b == idx);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            /* Delete reverse */
            multiarraySmallDelete(s, globalMax - idx - 1);
        }

        multiarraySmallFree(s);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
