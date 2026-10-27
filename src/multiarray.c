#include "multiarray.h"

#include "multiarrayLarge.h"
#include "multiarrayLargeInternal.h"

#include "multiarrayMedium.h"
#include "multiarrayMediumInternal.h"

#include "multiarraySmall.h"
#include "multiarraySmallInternal.h"

#if MULTIARRAY_INLINE
#include "multiarrayLarge.c"
#include "multiarrayMedium.c"
#include "multiarraySmall.c"
#endif

#include "cleaner.h"
#include "dkDiag.h"

#include <assert.h>

defcleaner(void *, zfree);

/* A container call given a native multiarray (its own macro API owns it):
 * refused with a defined answer, and reported as misuse
 * (DK_DIAG_MULTIARRAY_NATIVE).  NULL is refused the same way without a
 * report: NULL is every container call's "nothing" */
[[gnu::cold, gnu::noinline]] static void *maNativeRefused_(const char *function,
                                                           int line) {
    dkDiagReport_(DK_DIAG_MISUSE, DK_DIAG_MULTIARRAY_NATIVE, function, __FILE__,
                  line,
                  "a container call was given a native multiarray (use the "
                  "multiarrayNative* macros)");
    return nullptr;
}
#define MA_NATIVE_REFUSED_(m)                                                  \
    do {                                                                       \
        if ((m) != nullptr) {                                                  \
            (void)maNativeRefused_(__func__, __LINE__);                        \
        }                                                                      \
    } while (0)

#define mars(m) ((multiarraySmall *)_MULTIARRAY_USE(m))
#define marm(m) ((multiarrayMedium *)_MULTIARRAY_USE(m))
#define marl(m) ((multiarrayLarge *)_MULTIARRAY_USE(m))

#define _MULTIARRAY(ret, m, func, ...)                                         \
    do {                                                                       \
        switch (_multiarrayType(m)) {                                          \
        case MULTIARRAY_TYPE_SMALL:                                            \
            ret multiarraySmall##func(mars(m), __VA_ARGS__);                   \
            break;                                                             \
        case MULTIARRAY_TYPE_MEDIUM:                                           \
            ret multiarrayMedium##func(marm(m), __VA_ARGS__);                  \
            break;                                                             \
        case MULTIARRAY_TYPE_LARGE:                                            \
            ret multiarrayLarge##func(marl(m), __VA_ARGS__);                   \
            break;                                                             \
        default: /* native (or NULL): refused by the caller's fallback */      \
            MA_NATIVE_REFUSED_(m);                                             \
            break;                                                             \
        }                                                                      \
    } while (0)

#define MULTIARRAY_NORETURN(m, func, ...) _MULTIARRAY(, m, func, __VA_ARGS__)

#define _MULTIARRAY_SINGLE(ret, m, func)                                       \
    do {                                                                       \
        switch (_multiarrayType(m)) {                                          \
        case MULTIARRAY_TYPE_SMALL:                                            \
            ret multiarraySmall##func(mars(m));                                \
            break;                                                             \
        case MULTIARRAY_TYPE_MEDIUM:                                           \
            ret multiarrayMedium##func(marm(m));                               \
            break;                                                             \
        case MULTIARRAY_TYPE_LARGE:                                            \
            ret multiarrayLarge##func(marl(m));                                \
            break;                                                             \
        default: /* native (or NULL): refused by the caller's fallback */      \
            MA_NATIVE_REFUSED_(m);                                             \
            break;                                                             \
        }                                                                      \
    } while (0)

#define MULTIARRAY_SINGLE_NORETURN(m, func) _MULTIARRAY_SINGLE(, m, func)
#define MULTIARRAY_SINGLE_RETURN(m, func) _MULTIARRAY_SINGLE(return, m, func)

/* The container API (multiarrayNew) works on its own tagged tiers.  A
 * native multiarray (multiarrayNativeNew: a bare array whose element size
 * only its macro API knows) or NULL is refused with a defined answer:
 * readers give 0 / NULL, writers change nothing, Free does nothing.
 * Count/Bytes: the Native macro API tracks its count externally. */
size_t multiarrayCount(const multiarray *m) {
    MULTIARRAY_SINGLE_RETURN(m, Count);
    return 0;
}

size_t multiarrayBytes(const multiarray *m) {
    MULTIARRAY_SINGLE_RETURN(m, Bytes);
    return 0;
}

void multiarrayStats(const multiarray *m, dkTierStats *out) {
    *out = (dkTierStats){}; /* native or NULL: no struct, no count */
    switch (_multiarrayType(m)) {
    case MULTIARRAY_TYPE_SMALL: {
        const multiarraySmall *s = mars(m);
        const uint64_t data = (uint64_t)s->count * s->len;
        *out = (dkTierStats){.count = s->count,
                             .dataBytes = data,
                             .totalBytes = data + sizeof(*s),
                             .units = 1,
                             .unitLimit = (uint32_t)s->rowMax * s->len,
                             .tier = MULTIARRAY_TYPE_SMALL};
        break;
    }
    case MULTIARRAY_TYPE_MEDIUM: {
        const multiarrayMedium *md = marm(m);
        const uint64_t count = multiarrayMediumCount(md);
        const uint64_t data = count * md->len;
        *out = (dkTierStats){.count = count,
                             .dataBytes = data,
                             .totalBytes = data + sizeof(*md) +
                                           (uint64_t)md->count *
                                               sizeof(multiarrayMediumNode),
                             .units = md->count,
                             .unitLimit = (uint32_t)md->rowMax * md->len,
                             .tier = MULTIARRAY_TYPE_MEDIUM};
        break;
    }
    case MULTIARRAY_TYPE_LARGE:
        multiarrayLargeStats(marl(m), out);
        break;
    default:
        break;
    }
}

multiarray *multiarrayNew(uint16_t len, uint16_t rowMax) {
    multiarraySmall *mar = multiarraySmallNew(len, rowMax);
    return _MULTIARRAY_TAG(mar, MULTIARRAY_TYPE_SMALL);
}

/* Element 'idx' counted from the front on every tier (the tiers' own
 * negative-from-the-end form is theirs alone); NULL past the end */
void *multiarrayGet(multiarray *m, multiarrayIdx idx) {
    switch (_multiarrayType(m)) {
    case MULTIARRAY_TYPE_SMALL: {
        multiarraySmall *s = mars(m);
        return likely(idx < s->count) ? multiarraySmallGet(s, idx)
                                      : MA_INDEX_REFUSED_(idx);
    }
    case MULTIARRAY_TYPE_MEDIUM:
        return multiarrayMediumGetForward(marm(m), idx);
    case MULTIARRAY_TYPE_LARGE:
        return multiarrayLargeGetForward(marl(m), idx);
    default: /* native or NULL: refused (a tail call, as above) */
        return m ? maNativeRefused_(__func__, __LINE__) : nullptr;
    }
}

/* First / last element; NULL when empty (each tier's own rule) */
void *multiarrayGetHead(multiarray *m) {
    MULTIARRAY_SINGLE_RETURN(m, GetHead);
    return nullptr;
}

void *multiarrayGetTail(multiarray *m) {
    MULTIARRAY_SINGLE_RETURN(m, GetTail);
    return nullptr;
}

DK_STATIC inline void _multiarrayUpgrade(multiarray **m) {
    /* Now check multiarray to see if we should grow to a larger
     * representation. */
    multiarraySmall *small;
    multiarrayMedium *medium;
    multiarrayLarge *full;
    if (_multiarrayType(*m) == MULTIARRAY_TYPE_SMALL) {
        small = mars(*m);
        if (small->count == small->rowMax) {
            medium = multiarrayMediumFromSmall(small);
            *m = _MULTIARRAY_TAG(medium, MULTIARRAY_TYPE_MEDIUM);
        }
    } else if (_multiarrayType(*m) == MULTIARRAY_TYPE_MEDIUM) {
        medium = marm(*m);
        if (medium->count == medium->rowMax) {
            full = multiarrayLargeFromMedium(medium);
            *m = _MULTIARRAY_TAG(full, MULTIARRAY_TYPE_LARGE);
        }
    }
}

void multiarrayInsert(multiarray **m, multiarrayIdx idx, void *what) {
    /* Insert new elements */
    /* Upgrade *before* insert so we don't end up with dangling
     * containers (e.g. insert 512 items, upgrade container, but
     * never insert a 513 item, so now we have a big container
     * for no reason). */
    _multiarrayUpgrade(m);
    MULTIARRAY_NORETURN(*m, Insert, idx, what);
}

/* XOR-linked Large nodes: the node after 'node', walking from 'from' */
#define multiarrayLargeStep_(from, node)                                       \
    ((multiarrayLargeNode *)((uintptr_t)(from) ^ (uintptr_t)(node)->prevNext))

/* Large -> Medium: the nodes move, in list order, into one node array; their
 * element data is kept as it is */
[[gnu::noinline]] static multiarrayMedium *
multiarrayMediumFromLarge_(multiarrayLarge *large) {
    const uint32_t nodes = large->nodes;
    const uint16_t len = large->len;
    const uint16_t rowMax = large->rowMax;
    /* @own: container; free=multiarrayMediumFree */
    multiarrayMediumNode *node = zcalloc(nodes, sizeof(*node));
    uintptr_t prev = 0;
    multiarrayLargeNode *cur = large->head;
    for (uint32_t i = 0; cur; i++) {
        assert(i < nodes);
        node[i] = (multiarrayMediumNode){.data = cur->data,
                                         .len = len,
                                         .count = (uint16_t)cur->count,
                                         .rowMax = rowMax};
        multiarrayLargeNode *next = multiarrayLargeStep_(prev, cur);
        prev = (uintptr_t)cur;
        zfree(cur);
        cur = next;
    }

    /* @own: caller; free=multiarrayMediumFree */
    multiarrayMedium *med = zrealloc(large, sizeof(*med));
    *med = (multiarrayMedium){
        .node = node, .count = nodes, .len = len, .rowMax = rowMax};
    return med;
}

/* Medium -> Small: the 'total' elements of at most two nodes, joined in the
 * first node's buffer */
[[gnu::noinline]] static multiarraySmall *
multiarraySmallFromMedium_(multiarrayMedium *med, uint32_t total) {
    const size_t len = med->len;
    const uint16_t rowMax = med->rowMax;
    uint32_t at = med->node[0].count;
    /* @own: container; free=multiarraySmallFree */
    uint8_t *data = zrealloc(med->node[0].data, len * (total ? total : 1));
    for (uint32_t i = 1; i < med->count; i++) {
        const multiarrayMediumNode *n = &med->node[i];
        if (n->count) {
            memcpy(data + len * at, n->data, len * n->count);
            at += n->count;
        }
        zfree(n->data);
    }
    assert(at == total);
    zfree(med->node);

    /* @own: caller; free=multiarraySmallFree */
    multiarraySmall *s = zrealloc(med, sizeof(*s));
    *s = (multiarraySmall){.data = data,
                           .len = (uint16_t)len,
                           .count = (uint16_t)total,
                           .rowMax = rowMax};
    return s;
}

/* A drained array moves back down its tiers well below where it grew
 * (Small -> Medium at rowMax elements, Medium -> Large at rowMax nodes), so
 * it never flips at a boundary: Large holding rowMax/2 nodes or fewer
 * becomes Medium, and Medium holding rowMax/2 elements or fewer in at most
 * two nodes becomes Small.  Each tier's delete calls its own check, which
 * reads a count the tier keeps: Small pays nothing, Medium and Large one
 * compare when no tier changes. */
DK_STATIC inline void multiarrayShrinkMedium_(multiarray **m,
                                              multiarrayMedium *med) {
    if (med->count > 2) {
        return;
    }
    const uint32_t total =
        med->node[0].count + (med->count == 2 ? med->node[1].count : 0);
    if (total <= (uint32_t)med->rowMax / 2) {
        *m = _MULTIARRAY_TAG(multiarraySmallFromMedium_(med, total),
                             MULTIARRAY_TYPE_SMALL);
    }
}

DK_STATIC inline void multiarrayShrinkLarge_(multiarray **m,
                                             multiarrayLarge *large) {
    if (large->nodes <= (uint32_t)large->rowMax / 2) {
        multiarrayMedium *med = multiarrayMediumFromLarge_(large);
        *m = _MULTIARRAY_TAG(med, MULTIARRAY_TYPE_MEDIUM);
        multiarrayShrinkMedium_(m, med);
    }
}

void multiarrayDelete(multiarray **m, const uint32_t index) {
    multiarrayPop(m, index, nullptr);
}

void multiarrayPop(multiarray **m, multiarrayIdx idx, void *out) {
    switch (_multiarrayType(*m)) {
    case MULTIARRAY_TYPE_SMALL:
        multiarraySmallDeleteTaking(mars(*m), idx, out);
        break;
    case MULTIARRAY_TYPE_MEDIUM:
        multiarrayMediumDeleteTaking(marm(*m), (int32_t)idx, out);
        multiarrayShrinkMedium_(m, marm(*m));
        break;
    case MULTIARRAY_TYPE_LARGE:
        multiarrayLargeDeleteTaking(marl(*m), (int32_t)idx, out);
        multiarrayShrinkLarge_(m, marl(*m));
        break;
    default: /* native or NULL: refused, 'out' untouched */
        MA_NATIVE_REFUSED_(*m);
        break;
    }
}

void multiarrayDeleteRange(multiarray **m, multiarrayIdx start,
                           uint32_t count) {
    switch (_multiarrayType(*m)) {
    case MULTIARRAY_TYPE_SMALL:
        multiarraySmallDeleteRange(mars(*m), start, count);
        break;
    case MULTIARRAY_TYPE_MEDIUM:
        multiarrayMediumDeleteRange(marm(*m), start, count);
        multiarrayShrinkMedium_(m, marm(*m));
        break;
    case MULTIARRAY_TYPE_LARGE:
        multiarrayLargeDeleteRange(marl(*m), start, count);
        multiarrayShrinkLarge_(m, marl(*m));
        break;
    default: /* native or NULL: refused */
        MA_NATIVE_REFUSED_(*m);
        break;
    }
}

void multiarrayReplace(multiarray *m, multiarrayIdx idx, const void *what) {
    void *at = multiarrayGet(m, idx);
    if (at) {
        memcpy(at, what, multiarrayElementLen(m));
    }
}

void multiarrayRotate(multiarray **m) {
    const size_t count = multiarrayCount(*m);
    if (count < 2) {
        return;
    }

    /* The tail element, held while it moves to the front */
    const uint16_t len = multiarrayElementLen(*m);
    uint8_t onStack[64];
    cleaner(zfree) void *onHeap =
        len > sizeof(onStack) ? zmalloc(len) : nullptr;
    void *tail = onHeap ? onHeap : onStack;
    multiarrayPop(m, (multiarrayIdx)(count - 1), tail);
    multiarrayInsert(m, 0, tail);
}

void multiarrayIteratorInit(const multiarray *m, multiarrayIter *iter,
                            bool forward) {
    *iter = (multiarrayIter){.tier = _MULTIARRAY_USE(m),
                             .type = _multiarrayType(m),
                             .forward = forward};
    switch (_multiarrayType(m)) {
    case MULTIARRAY_TYPE_SMALL:
        iter->offset = forward ? 0 : (int32_t)mars(m)->count - 1;
        break;
    case MULTIARRAY_TYPE_MEDIUM: {
        const multiarrayMedium *med = marm(m);
        iter->nodeIdx = forward ? 0 : med->count - 1;
        iter->offset =
            forward ? 0 : (int32_t)med->node[iter->nodeIdx].count - 1;
        break;
    }
    case MULTIARRAY_TYPE_LARGE: {
        const multiarrayLarge *large = marl(m);
        iter->node = forward ? large->head : large->tail;
        iter->offset =
            forward
                ? 0
                : (int32_t)((const multiarrayLargeNode *)iter->node)->count - 1;
        break;
    }
    default: /* native or NULL: an iterator that yields nothing */
        MA_NATIVE_REFUSED_(m);
        break;
    }
}

void multiarrayIteratorInitAt(const multiarray *m, multiarrayIter *iter,
                              multiarrayIdx idx, bool forward) {
    *iter = (multiarrayIter){.tier = _MULTIARRAY_USE(m),
                             .type = _multiarrayType(m),
                             .forward = forward};
    if (_multiarrayType(m) == MULTIARRAY_TYPE_NATIVE) {
        MA_NATIVE_REFUSED_(m);
        return; /* native or NULL: an iterator that yields nothing */
    }
    /* Each locate stops in the end node for an index past the array, so
     * the landing offset tells an index outside it */
    uint32_t inNode = 0;
    switch (_multiarrayType(m)) {
    case MULTIARRAY_TYPE_SMALL:
        iter->offset = (int32_t)idx;
        inNode = mars(m)->count;
        break;
    case MULTIARRAY_TYPE_MEDIUM: {
        uint32_t offset;
        multiarrayMediumLocate(marm(m), idx, &iter->nodeIdx, &offset);
        iter->offset = (int32_t)offset;
        inNode = marm(m)->node[iter->nodeIdx].count;
        break;
    }
    case MULTIARRAY_TYPE_LARGE: {
        multiarrayLargeNode *prev;
        multiarrayLargeNode *node;
        uint32_t offset;
        multiarrayLargeLocate(marl(m), idx, &prev, &node, &offset);
        iter->node = node;
        /* Walking backwards, the node 'behind' is the one after */
        iter->prev = forward ? prev : multiarrayLargeStep_(prev, node);
        iter->offset = (int32_t)offset;
        inNode = node->count;
        break;
    }
    default: /* native: handled above */
        break;
    }

    /* Outside the array (or past int32_t): an iterator that yields
     * nothing */
    if (unlikely(iter->offset < 0 || (uint32_t)iter->offset >= inNode)) {
        MA_INDEX_REFUSED_(idx);
        *iter = (multiarrayIter){.type = MULTIARRAY_TYPE_NATIVE};
    }
}

void *multiarrayIteratorNext(multiarrayIter *iter) {
    const int32_t step = iter->forward ? 1 : -1;
    switch (iter->type) {
    case MULTIARRAY_TYPE_SMALL: {
        const multiarraySmall *s = iter->tier;
        if (iter->offset < 0 || iter->offset >= (int32_t)s->count) {
            return nullptr;
        }

        void *at = s->data + (size_t)s->len * (uint32_t)iter->offset;
        iter->offset += step;
        return at;
    }
    case MULTIARRAY_TYPE_MEDIUM: {
        const multiarrayMedium *med = iter->tier;
        while (true) {
            const multiarrayMediumNode *node = &med->node[iter->nodeIdx];
            if (iter->offset >= 0 && iter->offset < (int32_t)node->count) {
                void *at =
                    node->data + (size_t)med->len * (uint32_t)iter->offset;
                iter->offset += step;
                return at;
            }

            /* This node is done (or empty): the next one in the walk */
            if (iter->forward) {
                if (iter->nodeIdx + 1 >= med->count) {
                    return nullptr;
                }
                iter->nodeIdx++;
                iter->offset = 0;
            } else {
                if (iter->nodeIdx == 0) {
                    return nullptr;
                }
                iter->nodeIdx--;
                iter->offset = (int32_t)med->node[iter->nodeIdx].count - 1;
            }
        }
    }
    case MULTIARRAY_TYPE_LARGE: {
        const multiarrayLarge *large = iter->tier;
        while (iter->node) {
            const multiarrayLargeNode *node = iter->node;
            if (iter->offset >= 0 && iter->offset < (int32_t)node->count) {
                void *at =
                    node->data + (size_t)large->len * (uint32_t)iter->offset;
                iter->offset += step;
                return at;
            }

            const multiarrayLargeNode *after =
                multiarrayLargeStep_(iter->prev, node);
            iter->prev = node;
            iter->node = after;
            if (after) {
                iter->offset = iter->forward ? 0 : (int32_t)after->count - 1;
            }
        }

        return nullptr;
    }
    default: /* native or NULL */
        return nullptr;
    }
}

void multiarrayFree(multiarray *m) {
    MULTIARRAY_SINGLE_NORETURN(m, Free);
}

multiarray *multiarrayCopy(const multiarray *m) {
    switch (_multiarrayType(m)) {
    case MULTIARRAY_TYPE_SMALL: {
        const multiarraySmall *from = mars(m);
        /* @own: caller; free=multiarrayFree */
        multiarraySmall *to = zmemdup(from, sizeof(*from));
        /* @own: container; free=multiarrayFree */
        to->data = zmemdup(from->data, (size_t)from->len * from->count);
        return _MULTIARRAY_TAG(to, MULTIARRAY_TYPE_SMALL);
    }
    case MULTIARRAY_TYPE_MEDIUM: {
        const multiarrayMedium *from = marm(m);
        /* @own: caller; free=multiarrayFree */
        multiarrayMedium *to = zmemdup(from, sizeof(*from));
        /* @own: container; free=multiarrayFree */
        to->node = zmemdup(from->node, from->count * sizeof(*from->node));
        for (uint32_t i = 0; i < from->count; i++) {
            /* @own: container; free=multiarrayFree */
            to->node[i].data = zmemdup(from->node[i].data,
                                       (size_t)from->len * from->node[i].count);
        }
        return _MULTIARRAY_TAG(to, MULTIARRAY_TYPE_MEDIUM);
    }
    case MULTIARRAY_TYPE_LARGE: {
        const multiarrayLarge *from = marl(m);
        /* @own: caller; free=multiarrayFree */
        multiarrayLarge *to = zmemdup(from, sizeof(*from));
        to->head = nullptr;
        multiarrayLargeNode *made = nullptr;
        const multiarrayLargeNode *prev = nullptr;
        for (const multiarrayLargeNode *cur = from->head; cur;) {
            /* @own: container; free=multiarrayFree */
            multiarrayLargeNode *node = zcalloc(1, sizeof(*node));
            /* @own: container; free=multiarrayFree */
            node->data = zmemdup(cur->data, (size_t)from->len * cur->count);
            node->count = cur->count;
            /* XOR links: each new node points back at the one before, and
             * that one gains this one as its next */
            node->prevNext = (uintptr_t)made;
            if (made) {
                made->prevNext ^= (uintptr_t)node;
            } else {
                to->head = node;
            }
            made = node;

            const multiarrayLargeNode *next = multiarrayLargeStep_(prev, cur);
            prev = cur;
            cur = next;
        }
        to->tail = made;
        return _MULTIARRAY_TAG(to, MULTIARRAY_TYPE_LARGE);
    }
    default: /* native or NULL: refused */
        MA_NATIVE_REFUSED_(m);
        return nullptr;
    }
}

void multiarrayReset(multiarray **m) {
    switch (_multiarrayType(*m)) {
    case MULTIARRAY_TYPE_SMALL: {
        multiarraySmall *s = mars(*m);
        /* @own: container; free=multiarraySmallFree */
        s->data = zrealloc(s->data, s->len);
        s->count = 0;
        return;
    }
    case MULTIARRAY_TYPE_MEDIUM:
    case MULTIARRAY_TYPE_LARGE: {
        const uint16_t len = multiarrayElementLen(*m);
        const uint16_t rowMax = multiarrayRowMax(*m);
        multiarrayFree(*m);
        /* @own: caller; free=multiarrayFree */
        *m = multiarrayNew(len, rowMax);
        return;
    }
    default: /* native or NULL: refused */
        MA_NATIVE_REFUSED_(*m);
        return;
    }
}

uint16_t multiarrayElementLen(const multiarray *m) {
    switch (_multiarrayType(m)) {
    case MULTIARRAY_TYPE_SMALL:
        return mars(m)->len;
    case MULTIARRAY_TYPE_MEDIUM:
        return marm(m)->len;
    case MULTIARRAY_TYPE_LARGE:
        return marl(m)->len;
    default: /* native or NULL */
        MA_NATIVE_REFUSED_(m);
        return 0;
    }
}

uint16_t multiarrayRowMax(const multiarray *m) {
    switch (_multiarrayType(m)) {
    case MULTIARRAY_TYPE_SMALL:
        return mars(m)->rowMax;
    case MULTIARRAY_TYPE_MEDIUM:
        return marm(m)->rowMax;
    case MULTIARRAY_TYPE_LARGE:
        return marl(m)->rowMax;
    default: /* native or NULL */
        MA_NATIVE_REFUSED_(m);
        return 0;
    }
}

void multiarrayCopyTo(const multiarray *m, void *out) {
    uint8_t *to = out;
    switch (_multiarrayType(m)) {
    case MULTIARRAY_TYPE_SMALL: {
        const multiarraySmall *s = mars(m);
        if (s->count) {
            memcpy(to, s->data, (size_t)s->count * s->len);
        }
        break;
    }
    case MULTIARRAY_TYPE_MEDIUM: {
        const multiarrayMedium *med = marm(m);
        for (uint32_t i = 0; i < med->count; i++) {
            const size_t bytes = (size_t)med->node[i].count * med->len;
            if (bytes) {
                memcpy(to, med->node[i].data, bytes);
                to += bytes;
            }
        }
        break;
    }
    case MULTIARRAY_TYPE_LARGE: {
        const multiarrayLarge *large = marl(m);
        multiarrayLargeNode *prev = nullptr;
        multiarrayLargeNode *cur = large->head;
        while (cur) {
            const size_t bytes = (size_t)cur->count * large->len;
            memcpy(to, cur->data, bytes);
            to += bytes;
            /* XOR-linked: the next node is prev ^ cur->prevNext */
            multiarrayLargeNode *next =
                (multiarrayLargeNode *)((uintptr_t)prev ^
                                        (uintptr_t)cur->prevNext);
            prev = cur;
            cur = next;
        }
        break;
    }
    default: /* native or NULL: nothing copied */
        MA_NATIVE_REFUSED_(m);
        break;
    }
}

multiarray *multiarrayNewFromArray(uint16_t len, uint16_t rowMax,
                                   const void *elements, size_t count) {
    const uint8_t *from = elements;

    /* One node's worth: Small */
    if (count <= rowMax) {
        multiarraySmall *s = multiarraySmallNew(len, rowMax);
        if (count) {
            /* @own: container; free=multiarraySmallFree */
            s->data = zrealloc(s->data, count * len);
            memcpy(s->data, from, count * len);
            s->count = (uint16_t)count;
        }
        return _MULTIARRAY_TAG(s, MULTIARRAY_TYPE_SMALL);
    }

    /* Full nodes of rowMax entries (the last holds the rest) in a Medium,
     * which becomes Large at rowMax nodes, as inserts would make it */
    const size_t nodes = (count + rowMax - 1) / rowMax;
    /* @own: caller; free=multiarrayMediumFree */
    multiarrayMedium *med = zcalloc(1, sizeof(*med));
    /* @own: container; free=multiarrayMediumFree */
    med->node = zcalloc(nodes, sizeof(*med->node));
    med->count = (uint32_t)nodes;
    med->len = len;
    med->rowMax = rowMax;
    for (size_t i = 0; i < nodes; i++) {
        const size_t here = i + 1 < nodes ? rowMax : count - i * rowMax;
        /* @own: container; free=multiarrayMediumFree */
        med->node[i].data = zmalloc(here * len);
        memcpy(med->node[i].data, from + i * rowMax * (size_t)len, here * len);
        med->node[i].count = (uint16_t)here;
    }

    if (nodes >= rowMax) {
        multiarrayLarge *large = multiarrayLargeFromMedium(med);
        return _MULTIARRAY_TAG(large, MULTIARRAY_TYPE_LARGE);
    }
    return _MULTIARRAY_TAG(med, MULTIARRAY_TYPE_MEDIUM);
}

/* One node's elements, in order, as Compact reads them */
typedef struct multiarrayRun_ {
    const uint8_t *data;
    uint32_t count;
} multiarrayRun_;

/* Copy the next 'want' elements of 'run' (from element 'at' of run 'r')
 * to 'to', advancing the position */
static void multiarrayRunsTake_(const multiarrayRun_ *run, size_t *r,
                                uint32_t *at, size_t len, uint8_t *to,
                                size_t want) {
    while (want) {
        const uint32_t left = run[*r].count - *at;
        if (!left) {
            (*r)++;
            *at = 0;
            continue;
        }
        const uint32_t take = want < left ? (uint32_t)want : left;
        memcpy(to, run[*r].data + len * *at, len * take);
        to += len * take;
        *at += take;
        want -= take;
    }
}

void multiarrayCompact(multiarray **m) {
    const multiarrayType type = _multiarrayType(*m);
    if (type != MULTIARRAY_TYPE_MEDIUM && type != MULTIARRAY_TYPE_LARGE) {
        if (type == MULTIARRAY_TYPE_NATIVE) {
            MA_NATIVE_REFUSED_(*m);
        }
        return; /* a Small is one exact-fit array already */
    }

    /* The old nodes as runs, in order */
    const uint16_t len = multiarrayElementLen(*m);
    const uint16_t rowMax = multiarrayRowMax(*m);
    size_t runs;
    cleaner(zfree) void *runStorage = nullptr;
    multiarrayRun_ *run;
    if (type == MULTIARRAY_TYPE_MEDIUM) {
        const multiarrayMedium *med = marm(*m);
        runs = med->count;
        /* @own: caller; free=zfree */
        runStorage = zcalloc(runs, sizeof(*run));
        run = runStorage;
        for (size_t i = 0; i < runs; i++) {
            run[i] = (multiarrayRun_){med->node[i].data, med->node[i].count};
        }
    } else {
        const multiarrayLarge *large = marl(*m);
        runs = large->nodes;
        /* @own: caller; free=zfree */
        runStorage = zcalloc(runs, sizeof(*run));
        run = runStorage;
        const multiarrayLargeNode *prev = nullptr;
        size_t i = 0;
        for (const multiarrayLargeNode *cur = large->head; cur; i++) {
            run[i] = (multiarrayRun_){cur->data, (uint32_t)cur->count};
            const multiarrayLargeNode *next = multiarrayLargeStep_(prev, cur);
            prev = cur;
            cur = next;
        }
    }

    size_t count = 0;
    for (size_t i = 0; i < runs; i++) {
        count += run[i].count;
    }

    /* Full nodes of rowMax elements (the last holds the rest), in the tier
     * multiarrayNewFromArray would choose for them */
    size_t r = 0;
    uint32_t at = 0;
    multiarray *packed;
    if (count <= rowMax) {
        multiarraySmall *s = multiarraySmallNew(len, rowMax);
        /* @own: container; free=multiarraySmallFree */
        s->data = zrealloc(s->data, (size_t)len * (count ? count : 1));
        multiarrayRunsTake_(run, &r, &at, len, s->data, count);
        s->count = (uint16_t)count;
        packed = _MULTIARRAY_TAG(s, MULTIARRAY_TYPE_SMALL);
    } else {
        const size_t nodes = (count + rowMax - 1) / rowMax;
        /* @own: caller; free=multiarrayMediumFree */
        multiarrayMedium *med = zcalloc(1, sizeof(*med));
        /* @own: container; free=multiarrayMediumFree */
        med->node = zcalloc(nodes, sizeof(*med->node));
        med->count = (uint32_t)nodes;
        med->len = len;
        med->rowMax = rowMax;
        for (size_t i = 0; i < nodes; i++) {
            const size_t here = i + 1 < nodes ? rowMax : count - i * rowMax;
            /* @own: container; free=multiarrayMediumFree */
            med->node[i].data = zmalloc(here * len);
            multiarrayRunsTake_(run, &r, &at, len, med->node[i].data, here);
            med->node[i].count = (uint16_t)here;
        }
        packed = nodes >= rowMax
                     ? _MULTIARRAY_TAG(multiarrayLargeFromMedium(med),
                                       MULTIARRAY_TYPE_LARGE)
                     : _MULTIARRAY_TAG(med, MULTIARRAY_TYPE_MEDIUM);
    }

    multiarrayFree(*m);
    *m = packed;
}

#ifdef DATAKIT_TEST

#include "ctest.h"
#include "str.h"

typedef struct s16 {
    int64_t a;
    int64_t b;
} s16;

defcleanerName(s16 *, zfree, S16);
defcleaner(multiarray *, multiarrayFree);

/* Every element read back through iterators (both directions, from both
 * ends and from 'at') and CopyTo matches 'oracle' */
static int multiarrayOracleCheck_(const multiarray *m, const s16 *oracle,
                                  uint32_t n, uint32_t at, const char *after) {
    int err = 0;
    if (multiarrayCount(m) != n) {
        ERR("after %s: count %zu, oracle %" PRIu32, after, multiarrayCount(m),
            n);
        return err;
    }

    for (int32_t dir = 0; dir < 2; dir++) {
        multiarrayIter it;
        multiarrayIteratorInit(m, &it, dir == 0);
        uint32_t seen = 0;
        const s16 *e;
        while ((e = multiarrayIteratorNext(&it))) {
            const uint32_t want = dir == 0 ? seen : n - 1 - seen;
            if (seen >= n || e->a != oracle[want].a || e->b != oracle[want].b) {
                ERR("after %s: %s walk element %" PRIu32 " wrong", after,
                    dir == 0 ? "forward" : "backward", want);
                return err;
            }
            seen++;
        }

        if (seen != n) {
            ERR("after %s: %s walk saw %" PRIu32 " of %" PRIu32, after,
                dir == 0 ? "forward" : "backward", seen, n);
        }

        if (n == 0) {
            continue;
        }

        multiarrayIteratorInitAt(m, &it, at % n, dir == 0);
        uint32_t idx = at % n;
        while ((e = multiarrayIteratorNext(&it))) {
            if (e->a != oracle[idx].a) {
                ERR("after %s: walk from %" PRIu32 " wrong at %" PRIu32, after,
                    at % n, idx);
                return err;
            }
            if (dir == 0) {
                idx++;
            } else if (idx-- == 0) {
                break;
            }
        }
    }

    if (n) {
        cleanerName(zfree, S16) s16 *copy = zcalloc(n, sizeof(*copy));
        multiarrayCopyTo(m, copy);
        TEST_BYTES(err, copy, oracle, n * sizeof(*copy),
                   "after %s: CopyTo differs from the oracle", after);
    }

    return err;
}

/* Insert, Pop, DeleteRange, Replace and Rotate against a plain array,
 * holding the size inside [lo, hi] so the array stays in one tier while
 * every operation runs there */
static int multiarrayOracleSoak_(uint32_t lo, uint32_t hi, uint16_t rowMax,
                                 multiarrayType tier, uint64_t seed) {
    int err = 0;
    cleaner(multiarrayFree) multiarray *m = multiarrayNew(sizeof(s16), rowMax);
    cleanerName(zfree, S16) s16 *oracle = zcalloc(hi + 64, sizeof(*oracle));
    uint32_t n = 0;
    int64_t serial = 0;
    uint32_t inTier = 0;
    const uint32_t ops = 1500;
    for (uint32_t op = 0; op < ops && !err; op++) {
        inTier += _multiarrayType(m) == tier;
        dkLcg64Next(&seed);
        const uint32_t r = (uint32_t)(seed >> 33);
        const char *what;
        if (n < lo || (n < hi && r % 3 == 0)) {
            const uint32_t at = n ? r % (n + 1) : 0;
            s16 s = {.a = serial, .b = -serial};
            serial++;
            multiarrayInsert(&m, at, &s);
            memmove(&oracle[at + 1], &oracle[at], (n - at) * sizeof(s));
            oracle[at] = s;
            n++;
            what = "Insert";
        } else {
            switch (r % 4) {
            case 0: {
                const uint32_t at = r % n;
                s16 got;
                multiarrayPop(&m, at, &got);
                if (got.a != oracle[at].a || got.b != oracle[at].b) {
                    ERR("Pop(%" PRIu32 ") returned %" PRId64
                        ", oracle %" PRId64,
                        at, got.a, oracle[at].a);
                }
                memmove(&oracle[at], &oracle[at + 1],
                        (n - at - 1) * sizeof(*oracle));
                n--;
                what = "Pop";
                break;
            }
            case 1: {
                const uint32_t at = r % n;
                const uint32_t most = n - at < 2 * rowMax ? n - at : 2 * rowMax;
                const uint32_t count = (r >> 8) % (most + 1);
                multiarrayDeleteRange(&m, at, count);
                memmove(&oracle[at], &oracle[at + count],
                        (n - at - count) * sizeof(*oracle));
                n -= count;
                what = "DeleteRange";
                break;
            }
            case 2: {
                const uint32_t at = r % n;
                const s16 s = {.a = serial, .b = serial * 3};
                serial++;
                multiarrayReplace(m, at, &s);
                oracle[at] = s;
                what = "Replace";
                break;
            }
            default:
                multiarrayRotate(&m);
                if (n > 1) {
                    const s16 last = oracle[n - 1];
                    memmove(&oracle[1], &oracle[0], (n - 1) * sizeof(*oracle));
                    oracle[0] = last;
                }
                what = "Rotate";
                break;
            }
        }

        err += multiarrayOracleCheck_(m, oracle, n, r, what);
    }

    /* The size stays above the tier's shrink point, and fragmenting deletes
     * can promote early: most operations must still have run in the
     * intended tier */
    if (inTier < ops / 2) {
        ERR("soak in [%" PRIu32 ", %" PRIu32 "] ran %" PRIu32 " of %" PRIu32
            " operations in tier %d",
            lo, hi, inTier, ops, tier);
    }

    return err;
}

/* An array of 'n' elements (rowMax 8, so 'tier' is reached by size) given
 * indexes and ranges outside it: each Get answers NULL, each change is
 * refused with the array and 'out' unchanged, each reports
 * DK_DIAG_MULTIARRAY_INDEX exactly once; the edge calls just inside report
 * nothing */
static int multiarrayBoundsCheck_(uint32_t n, multiarrayType tier) {
    int err = 0;
    cleaner(multiarrayFree) multiarray *m = multiarrayNew(sizeof(s16), 8);
    cleanerName(zfree, S16) s16 *oracle = zcalloc(n + 1, sizeof(*oracle));
    for (uint32_t i = 0; i < n; i++) {
        oracle[i] = (s16){.a = i, .b = -(int64_t)i};
        multiarrayInsert(&m, i, &oracle[i]);
    }
    /* Empty arrays stay Small */
    if (_multiarrayType(m) != (n ? tier : MULTIARRAY_TYPE_SMALL)) {
        ERR("%" PRIu32 " elements built tier %d, want %d", n,
            _multiarrayType(m), tier);
        return err;
    }

    const dkDiagPrintMode priorPrint = dkDiagGetPrintMode();
    dkDiagSetPrintMode(DK_DIAG_PRINT_NEVER);
    char subject[32];
    snprintf(subject, sizeof(subject), "%" PRIu32 " elements", n);
    uint64_t want = 1;
#define MA_BOUNDS_(call)                                                       \
    DK_DIAG_EXPECT_REPORTS(subject, want, DK_DIAG_MULTIARRAY_INDEX, call)
    s16 probe = {.a = 99};
    s16 out = {.a = 77};
    const void *got[3] = {nullptr, nullptr, nullptr};
    multiarrayIter it;
    MA_BOUNDS_(got[0] = multiarrayGet(m, n));
    MA_BOUNDS_(got[1] = multiarrayGet(m, UINT32_MAX)); /* not "from the end" */
    MA_BOUNDS_(got[2] = multiarrayGet(m, 65536 + 1));
    MA_BOUNDS_(multiarrayInsert(&m, n + 1, &probe));
    MA_BOUNDS_(multiarrayInsert(&m, 65536 + 1, &probe)); /* not truncated */
    MA_BOUNDS_(multiarrayInsert(&m, UINT32_MAX, &probe));
    MA_BOUNDS_(multiarrayDelete(&m, n));
    MA_BOUNDS_(multiarrayDelete(&m, UINT32_MAX));
    MA_BOUNDS_(multiarrayPop(&m, n, &out));
    MA_BOUNDS_(multiarrayDeleteRange(&m, n, 1));
    MA_BOUNDS_(multiarrayDeleteRange(&m, 0, n + 1));
    MA_BOUNDS_(multiarrayDeleteRange(&m, n ? n - 1 : 0, 2));
    MA_BOUNDS_(multiarrayDeleteRange(&m, 1, UINT32_MAX));
    MA_BOUNDS_(multiarrayReplace(m, n, &probe));
    MA_BOUNDS_(multiarrayIteratorInitAt(m, &it, n, true));
    if (got[0] || got[1] || got[2] || out.a != 77 ||
        multiarrayIteratorNext(&it)) {
        ERR("%" PRIu32 " elements: an out-of-range call answered", n);
    }

    /* Just inside: nothing reported */
    want = 0;
    const void *head = nullptr;
    const void *tail = nullptr;
    const void *last = nullptr;
    MA_BOUNDS_(head = multiarrayGetHead(m));
    MA_BOUNDS_(tail = multiarrayGetTail(m));
    MA_BOUNDS_(multiarrayDeleteRange(&m, n, 0)); /* empty: no change */
    if (n) {
        MA_BOUNDS_(last = multiarrayGet(m, n - 1));
        /* Read before the append: an insert may move the elements */
        if (!head || !tail || tail != last || ((const s16 *)head)->a != 0 ||
            ((const s16 *)tail)->a != (int64_t)n - 1) {
            ERR("%" PRIu32 " elements: the edge elements are wrong", n);
        }
        MA_BOUNDS_(multiarrayInsert(&m, n, &probe)); /* the append */
        MA_BOUNDS_(multiarrayPop(&m, n, &out));
        if (out.a != 99) {
            ERR("%" PRIu32 " elements: the appended element did not pop", n);
        }
    } else if (head || tail) {
        ERR("an empty array has a head or tail");
    }
#undef MA_BOUNDS_
    dkDiagSetPrintMode(priorPrint);

    err += multiarrayOracleCheck_(m, oracle, n, n / 2, "bounds refusals");
    return err;
}

/* The tier rules once a change returns: Large only above rowMax/2 nodes,
 * Medium only while too big for Small */
static bool multiarrayTierSettled_(const multiarray *m) {
    dkTierStats s;
    multiarrayStats(m, &s);
    const uint32_t half = multiarrayRowMax(m) / 2;
    switch (s.tier) {
    case MULTIARRAY_TYPE_LARGE:
        return s.units > half;
    case MULTIARRAY_TYPE_MEDIUM:
        return s.units > 2 || s.count > half;
    default:
        return true;
    }
}

/* How a drain deletes */
typedef enum multiarrayDrainBy {
    MULTIARRAY_DRAIN_POP_TAIL,
    MULTIARRAY_DRAIN_DELETE_ANYWHERE,
    MULTIARRAY_DRAIN_DELETE_RANGES,
} multiarrayDrainBy;

/* Fill a rowMax-8 array to Large, then delete everything by 'by', checking
 * every step against an oracle and the tier rules.  Each time the tier
 * drops, one append must not grow it back (the hysteresis), and popping
 * that append restores the array.  The drain must pass through Medium and
 * end Small. */
static int multiarrayDrainCheck_(multiarrayDrainBy by, uint64_t seed) {
    int err = 0;
    const uint16_t rowMax = 8;
    const uint32_t fill = 400;
    cleaner(multiarrayFree) multiarray *m = multiarrayNew(sizeof(s16), rowMax);
    cleanerName(zfree, S16) s16 *oracle = zcalloc(fill + 1, sizeof(*oracle));
    for (uint32_t i = 0; i < fill; i++) {
        oracle[i] = (s16){.a = i, .b = -(int64_t)i};
        multiarrayInsert(&m, i, &oracle[i]);
    }
    if (_multiarrayType(m) != MULTIARRAY_TYPE_LARGE) {
        ERR("drain %d: %" PRIu32 " elements built tier %d", by, fill,
            _multiarrayType(m));
        return err;
    }

    uint32_t n = fill;
    bool sawMedium = false;
    while (n && !err) {
        dkLcg64Next(&seed);
        const uint32_t r = (uint32_t)(seed >> 33);
        const multiarrayType before = _multiarrayType(m);
        uint32_t at = r % n;
        uint32_t gone = 1;
        if (by == MULTIARRAY_DRAIN_POP_TAIL) {
            at = n - 1;
            s16 got;
            multiarrayPop(&m, at, &got);
            if (got.a != oracle[at].a) {
                ERR("drain %d: Pop(%" PRIu32 ") returned %" PRId64
                    ", oracle %" PRId64,
                    by, at, got.a, oracle[at].a);
            }
        } else if (by == MULTIARRAY_DRAIN_DELETE_ANYWHERE) {
            multiarrayDelete(&m, at);
        } else {
            const uint32_t most = n - at < 2u * rowMax ? n - at : 2u * rowMax;
            gone = 1 + (r >> 8) % most;
            multiarrayDeleteRange(&m, at, gone);
        }
        memmove(&oracle[at], &oracle[at + gone],
                (n - at - gone) * sizeof(*oracle));
        n -= gone;

        const multiarrayType after = _multiarrayType(m);
        sawMedium |= after == MULTIARRAY_TYPE_MEDIUM;
        if (!multiarrayTierSettled_(m)) {
            dkTierStats s;
            multiarrayStats(m, &s);
            ERR("drain %d: %" PRIu32 " elements in %u nodes left tier %d", by,
                n, s.units, after);
        }

        if (after < before) {
            s16 extra = {.a = -1, .b = -1};
            multiarrayInsert(&m, n, &extra);
            if (_multiarrayType(m) != after) {
                ERR("drain %d: one append after dropping to tier %d grew it "
                    "to %d",
                    by, after, _multiarrayType(m));
            }
            s16 back;
            multiarrayPop(&m, n, &back);
            if (back.a != -1) {
                ERR("drain %d: the append popped back as %" PRId64, by, back.a);
            }
        }

        err += multiarrayOracleCheck_(m, oracle, n, r, "a drain step");
    }

    if (!sawMedium || _multiarrayType(m) != MULTIARRAY_TYPE_SMALL) {
        ERR("drain %d: saw Medium %d, ended in tier %d", by, sawMedium,
            _multiarrayType(m));
    }

    return err;
}

int multiarrayTest(int, char *[]) {
    int err = 0;

    TEST("a drained array moves back down its tiers, never flapping") {
        err += multiarrayDrainCheck_(MULTIARRAY_DRAIN_POP_TAIL, 11);
        err += multiarrayDrainCheck_(MULTIARRAY_DRAIN_DELETE_ANYWHERE, 12);
        err += multiarrayDrainCheck_(MULTIARRAY_DRAIN_DELETE_RANGES, 13);
    }

    TEST("indexes and ranges outside the array are refused and reported, "
         "per tier") {
        err += multiarrayBoundsCheck_(0, MULTIARRAY_TYPE_SMALL);
        err += multiarrayBoundsCheck_(5, MULTIARRAY_TYPE_SMALL);
        err += multiarrayBoundsCheck_(40, MULTIARRAY_TYPE_MEDIUM);
        err += multiarrayBoundsCheck_(300, MULTIARRAY_TYPE_LARGE);
    }

    TEST("a Medium made from a full native array (an empty node after it) "
         "answers its ends and bounds") {
        /* The native macro API promotes a full bare array to Medium with
         * an empty node after the data */
        const uint16_t rowMax = 4;
        /* @own: container; free=multiarrayMediumFree */
        s16 *data = zcalloc(rowMax, sizeof(*data));
        for (uint16_t i = 0; i < rowMax; i++) {
            data[i] = (s16){.a = i};
        }
        /* @own: caller; free=multiarrayMediumFree */
        multiarrayMedium *med =
            multiarrayMediumNewWithData(sizeof(s16), rowMax, rowMax, data);
        const s16 *tail = multiarrayMediumGetTail(med);
        const s16 *back = multiarrayMediumGet(med, -1);
        const s16 *first = multiarrayMediumGet(med, -(int32_t)rowMax);
        const dkDiagPrintMode priorPrint = dkDiagGetPrintMode();
        dkDiagSetPrintMode(DK_DIAG_PRINT_NEVER);
        const uint64_t c0 = dkDiagCount();
        const void *past = multiarrayMediumGetForward(med, rowMax);
        const void *before = multiarrayMediumGet(med, -(int32_t)rowMax - 1);
        dkDiagSetPrintMode(priorPrint);
        if (!tail || tail->a != rowMax - 1 || back != tail || !first ||
            first->a != 0 || past || before || dkDiagCount() != c0 + 2) {
            ERR("a promoted Medium's ends or bounds are wrong");
        }

        /* And as a Large, which keeps the empty trailing node */
        /* @own: caller; free=multiarrayLargeFree */
        multiarrayLarge *large = multiarrayLargeFromMedium(med);
        const s16 *ltail = multiarrayLargeGetTail(large);
        if (!ltail || ltail->a != rowMax - 1 ||
            multiarrayLargeGet(large, -1) != ltail) {
            ERR("a Large carrying an empty trailing node lost its tail");
        }
        multiarrayLargeFree(large);
    }

    TEST("Insert / Pop / DeleteRange / Replace / Rotate / iterators vs an "
         "oracle, per tier") {
        /* rowMax 8: Small holds up to 8, Medium up to 8 nodes, then Large */
        err += multiarrayOracleSoak_(2, 7, 8, MULTIARRAY_TYPE_SMALL, 1);
        err += multiarrayOracleSoak_(12, 50, 8, MULTIARRAY_TYPE_MEDIUM, 2);
        err += multiarrayOracleSoak_(120, 400, 8, MULTIARRAY_TYPE_LARGE, 3);
    }

    TEST("Compact: same elements, full nodes, the tier their count needs") {
        const uint16_t rowMax = 8;
        cleaner(multiarrayFree) multiarray *m =
            multiarrayNew(sizeof(int64_t), rowMax);
        int64_t n = 0;
        for (; n < 600; n++) {
            multiarrayInsert(&m, (multiarrayIdx)n, &n);
        }

        cleaner(zfree) void *want = zcalloc((size_t)n, sizeof(int64_t));
        cleaner(zfree) void *got = zcalloc((size_t)n, sizeof(int64_t));
        const uint32_t keepEvery[] = {3, 5, 4, 30};
        for (size_t p = 0; p < countof(keepEvery); p++) {
            /* Back to front, so each index is still the element's */
            for (int64_t i = (int64_t)multiarrayCount(m) - 1; i >= 0; i--) {
                if (i % keepEvery[p]) {
                    multiarrayDelete(&m, (uint32_t)i);
                }
            }

            const size_t count = multiarrayCount(m);
            dkTierStats was;
            multiarrayStats(m, &was);
            multiarrayCopyTo(m, want);
            multiarrayCompact(&m);
            multiarrayCopyTo(m, got);
            dkTierStats s;
            multiarrayStats(m, &s);
            const size_t nodes = (count + rowMax - 1) / rowMax;
            const multiarrayType tier = count <= rowMax ? MULTIARRAY_TYPE_SMALL
                                        : nodes >= rowMax
                                            ? MULTIARRAY_TYPE_LARGE
                                            : MULTIARRAY_TYPE_MEDIUM;
            printf("  keep 1 in %2u: tier %u, %3u nodes, %5" PRIu64
                   " bytes -> tier %u, %3u, %5" PRIu64 "\n",
                   keepEvery[p], was.tier, was.units, was.totalBytes, s.tier,
                   s.units, s.totalBytes);
            if (multiarrayCount(m) != count ||
                memcmp(want, got, count * sizeof(int64_t)) != 0 ||
                _multiarrayType(m) != tier ||
                (tier != MULTIARRAY_TYPE_SMALL && s.units != nodes) ||
                s.totalBytes > was.totalBytes) {
                ERR("keep 1 in %u: %zu elements compacted to tier %d (want "
                    "%d), %u nodes (want %zu)",
                    keepEvery[p], count, _multiarrayType(m), tier, s.units,
                    nodes);
            }

            /* Still edits as before */
            int64_t v = -1;
            multiarrayInsert(&m, 0, &v);
            multiarrayPop(&m, 0, &v);
            if (v != -1 || multiarrayCount(m) != count) {
                ERR("keep 1 in %u: an edit after Compact went wrong",
                    keepEvery[p]);
            }
        }
    }

    TEST("Copy in every tier: same elements and tier, independent of the "
         "original") {
        for (int32_t tier = MULTIARRAY_TYPE_SMALL;
             tier <= MULTIARRAY_TYPE_LARGE; tier++) {
            cleaner(multiarrayFree) multiarray *m =
                multiarrayNew(sizeof(int64_t), 8);
            int64_t next = 0;
            do {
                multiarrayInsert(&m, (multiarrayIdx)(next / 2), &next);
                next++;
            } while (_multiarrayType(m) != (multiarrayType)tier);

            const size_t n = multiarrayCount(m);
            cleaner(multiarrayFree) multiarray *copy = multiarrayCopy(m);
            cleaner(zfree) void *want = zcalloc(n, sizeof(int64_t));
            cleaner(zfree) void *got = zcalloc(n, sizeof(int64_t));
            multiarrayCopyTo(m, want);
            multiarrayCopyTo(copy, got);
            dkTierStats a;
            dkTierStats b;
            multiarrayStats(m, &a);
            multiarrayStats(copy, &b);
            if (_multiarrayType(copy) != (multiarrayType)tier ||
                multiarrayCount(copy) != n || a.units != b.units ||
                memcmp(want, got, n * sizeof(int64_t)) != 0) {
                ERR("tier %d: copy differs (tier %d, %zu elements)", tier,
                    _multiarrayType(copy), multiarrayCount(copy));
            }

            /* Changing the copy leaves the original alone */
            int64_t v = -9;
            multiarrayReplace(copy, 0, &v);
            multiarrayPop(&copy, (multiarrayIdx)(n - 1), &v);
            multiarrayCopyTo(m, got);
            if (multiarrayCount(m) != n ||
                memcmp(want, got, n * sizeof(int64_t)) != 0) {
                ERR("tier %d: changing the copy changed the original", tier);
            }
        }
        cleaner(multiarrayFree) multiarray *ofNull = multiarrayCopy(nullptr);
        if (ofNull) {
            ERR("a copy of NULL is not NULL");
        }
    }

    TEST("Reset from every tier gives an empty Small of the same shape") {
        for (int32_t tier = MULTIARRAY_TYPE_SMALL;
             tier <= MULTIARRAY_TYPE_LARGE; tier++) {
            cleaner(multiarrayFree) multiarray *m =
                multiarrayNew(sizeof(int64_t), 8);
            int64_t next = 0;
            do {
                multiarrayInsert(&m, (multiarrayIdx)next, &next);
                next++;
            } while (_multiarrayType(m) != (multiarrayType)tier);

            multiarrayReset(&m);
            if (_multiarrayType(m) != MULTIARRAY_TYPE_SMALL ||
                multiarrayCount(m) != 0 || multiarrayGetHead(m) ||
                multiarrayElementLen(m) != sizeof(int64_t) ||
                multiarrayRowMax(m) != 8) {
                ERR("tier %d: reset to tier %d with %zu elements", tier,
                    _multiarrayType(m), multiarrayCount(m));
            }
            int64_t v = 41;
            multiarrayInsert(&m, 0, &v);
            const int64_t *got = multiarrayGet(m, 0);
            if (multiarrayCount(m) != 1 || !got || *got != 41) {
                ERR("tier %d: an element after Reset is missing", tier);
            }
        }
    }

    TEST("stats: tier, nodes, elements and bytes in every tier") {
        cleaner(multiarrayFree) multiarray *m =
            multiarrayNew(sizeof(int64_t), 8);
        dkTierStats s;
        int64_t next = 0;
        for (int32_t want = MULTIARRAY_TYPE_SMALL;
             want <= MULTIARRAY_TYPE_LARGE; want++) {
            while (_multiarrayType(m) != (multiarrayType)want) {
                multiarrayInsert(&m, (multiarrayIdx)next, &next);
                next++;
            }
            multiarrayStats(m, &s);
            bool shape;
            if (want == MULTIARRAY_TYPE_SMALL) {
                shape = s.units == 1 &&
                        s.totalBytes == s.dataBytes + sizeof(multiarraySmall);
            } else if (want == MULTIARRAY_TYPE_MEDIUM) {
                shape =
                    s.units == marm(m)->count &&
                    s.totalBytes == s.dataBytes + sizeof(multiarrayMedium) +
                                        s.units * sizeof(multiarrayMediumNode);
            } else {
                shape = s.units >= 2 && s.totalBytes > s.dataBytes;
            }
            if (s.tier != want || s.count != (uint64_t)next ||
                s.dataBytes != multiarrayBytes(m) ||
                s.unitLimit != 8 * sizeof(int64_t) || !shape) {
                ERR("tier %d: stats tier %u units %u count %" PRIu64
                    " limit %u data %" PRIu64 " total %" PRIu64,
                    want, s.tier, s.units, s.count, s.unitLimit, s.dataBytes,
                    s.totalBytes);
            }
        }
        multiarrayStats(nullptr, &s);
        if (s.tier || s.count || s.units || s.totalBytes) {
            ERR("NULL reports tier %u count %" PRIu64, s.tier, s.count);
        }
    }

    TEST("the container API refuses a native multiarray and NULL") {
        /* A native array (its element size known only to its macro API)
         * holding one element, and NULL: readers answer 0 / NULL, writers
         * and Free change nothing, iterators yield nothing, and the native
         * element is untouched */
        multiarray *nat = multiarrayNativeNew(s16);
        int count = 0;
        s16 one = {.a = 11};
        multiarrayNativeInsert(nat, s16, 64, count, 0, &one);
        multiarray *const subjects[2] = {nat, nullptr};
        const dkDiagPrintMode priorPrint = dkDiagGetPrintMode();
        dkDiagSetPrintMode(DK_DIAG_PRINT_NEVER);
        for (size_t i = 0; i < 2; i++) {
            multiarray *m = subjects[i];
            s16 probe = {.a = 99};
            s16 out = {.a = 77};
            uint8_t copied[sizeof(s16)] = {0xEE};
            /* A call on the native array reports the misuse once; one on
             * NULL ("nothing") reports nothing */
            const uint64_t want = i == 0 ? 1 : 0;
            const char *subject = i ? "NULL" : "a native multiarray";
#define MA_REPORTS_(call)                                                      \
    DK_DIAG_EXPECT_REPORTS(subject, want, DK_DIAG_MULTIARRAY_NATIVE, call)
            multiarrayIter it;
            const void *first = nullptr;
            const void *firstAt = nullptr;
            size_t counted = 0;
            size_t bytes = 0;
            const void *got[3] = {nullptr, nullptr, nullptr};
            uint16_t lens[2] = {0, 0};
            MA_REPORTS_(multiarrayIteratorInit(m, &it, true));
            first = multiarrayIteratorNext(&it);
            MA_REPORTS_(multiarrayIteratorInitAt(m, &it, 0, false));
            firstAt = multiarrayIteratorNext(&it);
            MA_REPORTS_(multiarrayInsert(&m, 0, &probe));
            MA_REPORTS_(multiarrayDelete(&m, 0));
            MA_REPORTS_(multiarrayPop(&m, 0, &out));
            MA_REPORTS_(multiarrayDeleteRange(&m, 0, 1));
            MA_REPORTS_(multiarrayReplace(m, 0, &probe));
            MA_REPORTS_(multiarrayRotate(&m));
            MA_REPORTS_(multiarrayCopyTo(m, copied));
            MA_REPORTS_(counted = multiarrayCount(m));
            MA_REPORTS_(bytes = multiarrayBytes(m));
            MA_REPORTS_(got[0] = multiarrayGet(m, 0));
            MA_REPORTS_(got[1] = multiarrayGetHead(m));
            MA_REPORTS_(got[2] = multiarrayGetTail(m));
            MA_REPORTS_(lens[0] = multiarrayElementLen(m));
            MA_REPORTS_(lens[1] = multiarrayRowMax(m));
#undef MA_REPORTS_
            if (counted || bytes || got[0] || got[1] || got[2] || lens[0] ||
                lens[1] || first || firstAt || out.a != 77 ||
                copied[0] != 0xEE || m != subjects[i]) {
                ERR("the container API acted on %s",
                    i ? "NULL" : "a native multiarray");
            }
            multiarrayFree(nullptr);
        }
        dkDiagSetPrintMode(priorPrint);
        s16 *held = nullptr;
        multiarrayNativeGet(nat, s16, held, 0);
        if (!held || held->a != 11) {
            ERR("the native element changed");
        }
        multiarrayNativeFree(nat);
    }

    printf("Testing direct...\n");
    /* Small up to rowMax, Medium up to rowMax * rowMax, Large beyond: three
     * times the Large threshold covers every tier and its transitions */
    static constexpr int rowMax = 64;
    static constexpr int globalMax = 3 * 64 * 64;
    TEST("create") {
        multiarray *s = multiarrayNativeNew(s16);
        assert(s);

        int count = 0;
        int idx = 0;
        s16 _s = {};
        multiarrayNativeInsert(s, s16, rowMax, count, idx, &_s);

        multiarrayNativeFree(s);
    }

    TEST("insert before") {
        multiarray *s = multiarrayNativeNew(s16);
        assert(s);

        int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            s16 _s = {.a = idx, .b = idx};
            multiarrayNativeInsert(s, s16, rowMax, count, idx, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            s16 *got = nullptr;
            multiarrayNativeGet(s, s16, got, idx);
            assert(got->a == idx);
            assert(got->b == idx);
        }

        multiarrayNativeFree(s);
    }

    TEST("insert before constant zero") {
        multiarray *s = multiarrayNativeNew(s16);
        assert(s);

        int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            s16 _s = {.a = idx, .b = idx};
            multiarrayNativeInsert(s, s16, rowMax, count, 0, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            s16 *got = nullptr;
            multiarrayNativeGet(s, s16, got, idx);
            assert(got->a == globalMax - 1 - idx);
            assert(got->b == globalMax - 1 - idx);
        }

        multiarrayNativeFree(s);
    }

    TEST("insert after") {
        multiarray *s = multiarrayNativeNew(s16);
        assert(s);

        int count = 0;

        s16 __s = {.a = 0, .b = 0};
        multiarrayNativeInsert(s, s16, rowMax, count, 0, &__s);
        for (int idx = 0; idx < globalMax; idx++) {
            s16 _s = {.a = idx, .b = idx};
            multiarrayNativeInsert(s, s16, rowMax, count, idx + 1, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            s16 *got = nullptr;
            multiarrayNativeGet(s, s16, got, idx + 1);
            assert(got->a == idx);
            assert(got->b == idx);
        }

        multiarrayNativeFree(s);
    }

    TEST("insert before randoms") {
        multiarray *s = multiarrayNativeNew(s16);
        assert(s);

        int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            s16 _s = {.a = idx, .b = idx};
            int32_t i = rand() % (idx + 1);
            multiarrayNativeInsert(s, s16, rowMax, count, i, &_s);
        }

        multiarrayNativeFree(s);
    }

    TEST("insert after") {
        multiarray *s = multiarrayNativeNew(s16);
        assert(s);

        int count = 0;

        s16 __s = {.a = 0, .b = 0};
        multiarrayNativeInsert(s, s16, rowMax, count, 0, &__s);
        for (int idx = 0; idx < globalMax; idx++) {
            s16 _s = {.a = idx, .b = idx};
            multiarrayNativeInsert(s, s16, rowMax, count, idx + 1, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            s16 *got = nullptr;
            multiarrayNativeGet(s, s16, got, idx + 1);
            assert(got->a == idx);
            assert(got->b == idx);
        }

        multiarrayNativeFree(s);
    }

    printf("Testing container...\n");
    TEST("create") {
        cleaner(multiarrayFree) multiarray *s =
            multiarrayNew(sizeof(s16), rowMax);
        assert(s);

        int idx = 0;
        s16 _s = {};
        multiarrayInsert(&s, idx, &_s);
    }

    TEST("insert before") {
        cleaner(multiarrayFree) multiarray *s =
            multiarrayNew(sizeof(s16), rowMax);
        assert(s);

        s16 _s = {};
        for (int idx = 0; idx < globalMax; idx++) {
            _s.a = idx;
            _s.b = idx;
            multiarrayInsert(&s, idx, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            assert(((s16 *)multiarrayGet(s, idx))->a == idx);
            assert(((s16 *)multiarrayGet(s, idx))->b == idx);
        }
    }

    TEST("insert before constant zero") {
        cleaner(multiarrayFree) multiarray *s =
            multiarrayNew(sizeof(s16), rowMax);
        assert(s);

        s16 _s = {};
        for (int idx = 0; idx < globalMax; idx++) {
            _s.a = idx;
            _s.b = idx;
            multiarrayInsert(&s, 0, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            assert(((s16 *)multiarrayGet(s, idx))->a == globalMax - 1 - idx);
            assert(((s16 *)multiarrayGet(s, idx))->b == globalMax - 1 - idx);
        }
    }

    TEST("insert before randoms") {
        cleaner(multiarrayFree) multiarray *s =
            multiarrayNew(sizeof(s16), rowMax);
        assert(s);

        for (int idx = 0; idx < globalMax; idx++) {
            s16 _s = {.a = idx, .b = idx};
            multiarrayInsert(&s, rand() % (idx + 1), &_s);
        }
    }

    TEST("insert after") {
        cleaner(multiarrayFree) multiarray *s =
            multiarrayNew(sizeof(s16), rowMax);
        assert(s);

        s16 _s = {};
        multiarrayInsert(&s, 0, &_s);
        for (int idx = 0; idx < globalMax; idx++) {
            _s.a = idx;
            _s.b = idx;
            multiarrayInsert(&s, idx + 1, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            assert(((s16 *)multiarrayGet(s, idx + 1))->a == idx);
            assert(((s16 *)multiarrayGet(s, idx + 1))->b == idx);
        }
    }

    /* ========================================
     * FUZZ TESTS - Oracle-Based Verification
     * ======================================== */
    printf("\n=== MULTIARRAY FUZZ TESTING ===\n\n");

    TEST("FUZZ: random insertions with oracle verification") {
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), rowMax);

        srand(12345);
        const int numOps = 10000;

        /* Oracle is a simple array */
        cleanerName(zfree, S16) s16 *oracle = zmalloc(numOps * sizeof(s16));
        int oracleCount = 0;

        for (int i = 0; i < numOps; i++) {
            int idx = (oracleCount > 0) ? (rand() % (oracleCount + 1)) : 0;
            s16 val = {.a = i, .b = i * 2};

            /* Insert into multiarray */
            multiarrayInsert(&mar, idx, &val);

            /* Insert into oracle */
            memmove(&oracle[idx + 1], &oracle[idx],
                    (oracleCount - idx) * sizeof(s16));
            oracle[idx] = val;
            oracleCount++;

            /* Periodic verification */
            if (i % 1000 == 0) {
                for (int j = 0; j < oracleCount; j++) {
                    s16 *got = (s16 *)multiarrayGet(mar, j);
                    if (got->a != oracle[j].a || got->b != oracle[j].b) {
                        ERR("Random insert at round %d idx %d: got (%" PRId64
                            ",%" PRId64 ") "
                            "expected (%" PRId64 ",%" PRId64 ")",
                            i, j, got->a, got->b, oracle[j].a, oracle[j].b);
                    }
                }
            }
        }

        /* Final verification */
        int mismatches = 0;
        for (int j = 0; j < oracleCount; j++) {
            s16 *got = (s16 *)multiarrayGet(mar, j);
            if (got->a != oracle[j].a || got->b != oracle[j].b) {
                if (mismatches < 5) {
                    ERR("Final verify idx %d: got (%" PRId64 ",%" PRId64
                        ") expected "
                        "(%" PRId64 ",%" PRId64 ")",
                        j, got->a, got->b, oracle[j].a, oracle[j].b);
                }
                mismatches++;
            }
        }

        printf("  %d random insertions, %d mismatches\n", numOps, mismatches);
    }

    TEST("FUZZ: tier transitions - small to medium to large") {
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), rowMax);

        const int targetCount =
            rowMax * 4; /* Force multiple tier transitions */

        for (int i = 0; i < targetCount; i++) {
            s16 val = {.a = i, .b = -i};
            multiarrayInsert(&mar, i, &val);

            /* Verify at tier boundaries */
            if (i == rowMax - 1 || i == rowMax || i == rowMax + 1 ||
                i == rowMax * 2 - 1 || i == rowMax * 2 || i == rowMax * 2 + 1) {
                /* Verify all elements so far */
                for (int j = 0; j <= i; j++) {
                    s16 *got = (s16 *)multiarrayGet(mar, j);
                    if (got->a != j || got->b != -j) {
                        ERR("Tier transition at %d, verify %d: got (%" PRId64
                            ",%" PRId64 ") "
                            "expected (%d,%d)",
                            i, j, got->a, got->b, j, -j);
                    }
                }
            }
        }

        /* Final verification */
        for (int i = 0; i < targetCount; i++) {
            s16 *got = (s16 *)multiarrayGet(mar, i);
            if (got->a != i || got->b != -i) {
                ERR("Final at %d: got (%" PRId64 ",%" PRId64
                    ") expected (%d,%d)",
                    i, got->a, got->b, i, -i);
            }
        }

        printf("  inserted %d elements through tier transitions\n",
               targetCount);
    }

    TEST("Count/Bytes: exact across all tiers, inserts and deletes") {
        /* Small rowMax so the sweep is cheap: Small upgrades to Medium
         * at rowMax ELEMENTS, but Medium upgrades to Large only at
         * rowMax NODES (rowMax^2 elements) — with 8 that's 8 and 64. */
        const int tinyRowMax = 8;
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), tinyRowMax);
        const int targetCount = tinyRowMax * tinyRowMax * 2;

        /* Empty container reports zero */
        assert(multiarrayCount(mar) == 0);
        assert(multiarrayBytes(mar) == 0);

        bool sawSmall = false;
        bool sawMedium = false;
        bool sawLarge = false;
        for (int i = 0; i < targetCount; i++) {
            s16 val = {.a = i, .b = -i};
            multiarrayInsert(&mar, i, &val);

            const size_t expect = (size_t)i + 1;
            if (multiarrayCount(mar) != expect) {
                ERR("Count after insert %d: got %zu expected %zu", i,
                    multiarrayCount(mar), expect);
            }

            assert(multiarrayBytes(mar) == expect * sizeof(s16));

            switch (_multiarrayType(mar)) {
            case MULTIARRAY_TYPE_SMALL:
                sawSmall = true;
                break;
            case MULTIARRAY_TYPE_MEDIUM:
                sawMedium = true;
                break;
            case MULTIARRAY_TYPE_LARGE:
                sawLarge = true;
                break;
            default:
                assert(false);
            }
        }

        /* The sweep must exercise every tier or the accessors are only
         * partially covered. */
        assert(sawSmall && sawMedium && sawLarge);

        /* Deletes shrink the count symmetrically (Large tier). */
        size_t remaining = targetCount;
        while (remaining > (size_t)tinyRowMax) {
            multiarrayDelete(&mar, 0);
            remaining--;
            assert(multiarrayCount(mar) == remaining);
            assert(multiarrayBytes(mar) == remaining * sizeof(s16));
        }

        printf("  Count/Bytes verified across %d inserts + deletes\n",
               targetCount);
    }

    TEST("FUZZ: insert at head repeatedly") {
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), rowMax);

        const int count = 5000;

        for (int i = 0; i < count; i++) {
            s16 val = {.a = i, .b = i};
            multiarrayInsert(&mar, 0, &val); /* Always insert at head */
        }

        /* Verify - elements should be in reverse order */
        for (int i = 0; i < count; i++) {
            s16 *got = (s16 *)multiarrayGet(mar, i);
            int expected = count - 1 - i;
            if (got->a != expected || got->b != expected) {
                ERR("Head insert at %d: got (%" PRId64 ",%" PRId64
                    ") expected (%d,%d)",
                    i, got->a, got->b, expected, expected);
            }
        }

        printf("  %d head insertions verified\n", count);
    }

    TEST("FUZZ: insert at tail repeatedly") {
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), rowMax);

        const int count = 5000;

        for (int i = 0; i < count; i++) {
            s16 val = {.a = i, .b = i};
            multiarrayInsert(&mar, i, &val); /* Insert at tail */
        }

        /* Verify - elements should be in order */
        for (int i = 0; i < count; i++) {
            s16 *got = (s16 *)multiarrayGet(mar, i);
            if (got->a != i || got->b != i) {
                ERR("Tail insert at %d: got (%" PRId64 ",%" PRId64
                    ") expected (%d,%d)",
                    i, got->a, got->b, i, i);
            }
        }

        printf("  %d tail insertions verified\n", count);
    }

    TEST("FUZZ: alternating head/tail insertions") {
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), rowMax);

        const int count = 4000;
        int currentCount = 0;

        for (int i = 0; i < count; i++) {
            s16 val = {.a = i, .b = i};
            if (i % 2 == 0) {
                multiarrayInsert(&mar, 0, &val); /* Head */
            } else {
                multiarrayInsert(&mar, currentCount, &val); /* Tail */
            }
            currentCount++;
        }

        /* Verify we can read all elements */
        for (int i = 0; i < count; i++) {
            s16 *got = (s16 *)multiarrayGet(mar, i);
            if (got == nullptr) {
                ERR("Alternating insert: NULL at idx %d", i);
            }
        }

        printf("  %d alternating insertions verified\n", count);
    }

    TEST("FUZZ: native API with oracle") {
        multiarray *s = multiarrayNativeNew(s16);
        int count = 0;

        srand(54321);
        const int numOps = 5000;

        /* Oracle */
        cleanerName(zfree, S16) s16 *oracle = zmalloc(numOps * sizeof(s16));
        int oracleCount = 0;

        for (int i = 0; i < numOps; i++) {
            int idx = (oracleCount > 0) ? (rand() % (oracleCount + 1)) : 0;
            s16 val = {.a = i, .b = i * 3};

            /* Insert into multiarray */
            multiarrayNativeInsert(s, s16, rowMax, count, idx, &val);

            /* Insert into oracle */
            memmove(&oracle[idx + 1], &oracle[idx],
                    (oracleCount - idx) * sizeof(s16));
            oracle[idx] = val;
            oracleCount++;
        }

        /* Verify all elements */
        int mismatches = 0;
        for (int j = 0; j < oracleCount; j++) {
            s16 *got = nullptr;
            multiarrayNativeGet(s, s16, got, j);
            if (got->a != oracle[j].a || got->b != oracle[j].b) {
                if (mismatches < 5) {
                    ERR("Native API verify idx %d: got (%" PRId64 ",%" PRId64
                        ") expected "
                        "(%" PRId64 ",%" PRId64 ")",
                        j, got->a, got->b, oracle[j].a, oracle[j].b);
                }
                mismatches++;
            }
        }

        printf("  %d native API insertions, %d mismatches\n", numOps,
               mismatches);
        multiarrayNativeFree(s);
    }

    TEST("FUZZ: stress test - 100K insertions") {
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), rowMax);

        srand(88888);
        const int numOps = 100'000;
        int currentCount = 0;

        for (int i = 0; i < numOps; i++) {
            int idx = (currentCount > 0) ? (rand() % (currentCount + 1)) : 0;
            s16 val = {.a = i, .b = -i};
            multiarrayInsert(&mar, idx, &val);
            currentCount++;

            /* Spot check periodically */
            if (i % 10000 == 0 && currentCount > 0) {
                int checkIdx = rand() % currentCount;
                s16 *got = (s16 *)multiarrayGet(mar, checkIdx);
                if (got == nullptr) {
                    ERR("100K stress: NULL at round %d, idx %d", i, checkIdx);
                }
            }
        }

        /* Sample verification - check 1000 random indices */
        int verified = 0;
        for (int i = 0; i < 1000; i++) {
            int idx = rand() % currentCount;
            s16 *got = (s16 *)multiarrayGet(mar, idx);
            if (got != nullptr) {
                verified++;
            } else {
                ERR("100K stress final: NULL at idx %d", idx);
            }
        }

        printf("  100K insertions completed, sampled %d indices\n", verified);
    }

    printf("\n=== All multiarray fuzz tests completed! ===\n\n");

    TEST("NewFromArray and CopyTo round trip in every tier; edits agree") {
        /* Counts that make Small, Medium and Large at rowMax 8; the bulk
         * build copies out the same elements, keeps its shape accessors,
         * head and tail, and later inserts and deletes leave it equal to
         * an insert-built twin */
        static constexpr size_t counts[] = {0, 5, 8, 9, 40, 63, 64, 65, 500};
        const uint16_t perNode = 8;
        for (size_t c = 0; c < countof(counts); c++) {
            const size_t n = counts[c];
            cleanerName(zfree, S16) s16 *want = zcalloc(n + 1, sizeof(s16));
            cleaner(multiarrayFree) multiarray *twin =
                multiarrayNew(sizeof(s16), perNode);
            for (size_t i = 0; i < n; i++) {
                want[i] = (s16){.a = (int64_t)i, .b = -(int64_t)i * 3};
                multiarrayInsert(&twin, (multiarrayIdx)i, &want[i]);
            }

            cleaner(multiarrayFree) multiarray *bulk =
                multiarrayNewFromArray(sizeof(s16), perNode, want, n);
            cleanerName(zfree, S16) s16 *got = zcalloc(n + 1, sizeof(s16));
            multiarrayCopyTo(bulk, got);
            if (multiarrayCount(bulk) != n ||
                memcmp(got, want, n * sizeof(s16)) != 0 ||
                multiarrayElementLen(bulk) != sizeof(s16) ||
                multiarrayRowMax(bulk) != perNode) {
                ERR("count %zu: bulk build differs", n);
                continue;
            }
            if (n &&
                (memcmp(multiarrayGetHead(bulk), &want[0], sizeof(s16)) != 0 ||
                 memcmp(multiarrayGetTail(bulk), &want[n - 1], sizeof(s16)) !=
                     0)) {
                ERR("count %zu: head or tail wrong", n);
            }

            /* The same edits to both */
            uint64_t rng = 0xa77a + n;
            for (int e = 0; e < 60; e++) {
                const size_t count = multiarrayCount(twin);
                const uint64_t at = splitmix64(&rng);
                if (count && at % 3 == 0) {
                    multiarrayDelete(&twin, (uint32_t)(at % count));
                    multiarrayDelete(&bulk, (uint32_t)(at % count));
                } else {
                    s16 v = {.a = e, .b = (int64_t)at};
                    multiarrayInsert(&twin, (multiarrayIdx)(at % (count + 1)),
                                     &v);
                    multiarrayInsert(&bulk, (multiarrayIdx)(at % (count + 1)),
                                     &v);
                }
            }
            const size_t after = multiarrayCount(twin);
            cleanerName(zfree, S16) s16 *a = zcalloc(after + 1, sizeof(s16));
            cleanerName(zfree, S16) s16 *b = zcalloc(after + 1, sizeof(s16));
            multiarrayCopyTo(twin, a);
            multiarrayCopyTo(bulk, b);
            if (multiarrayCount(bulk) != after ||
                memcmp(a, b, after * sizeof(s16)) != 0) {
                ERR("count %zu: edits disagree after a bulk build", n);
            }
        }
    }

    TEST_FINAL_RESULT;
}

#endif
