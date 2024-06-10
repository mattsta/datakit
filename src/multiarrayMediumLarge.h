#pragma once

#define multiarrayMediumLargeSplitNew(node, s, len)                            \
    do {                                                                       \
        (node)->data = zcalloc(1, len);                                        \
        (node)->count = 1;                                                     \
        memcpy((node)->data, s, len);                                          \
    } while (0)

#define multiarrayMediumLargeInsertAtIdx(node, remaining, remainingLen,        \
                                         offsetLen, count, s, len)             \
    do {                                                                       \
        (node)->data = zrealloc((node)->data, (len) * ((count) + 1));          \
        if (remaining) {                                                       \
            memmove((node)->data + (offsetLen) + (len),                        \
                    (node)->data + (offsetLen), remainingLen);                 \
        }                                                                      \
        memcpy((node)->data + (offsetLen), s, len);                            \
    } while (0)

#define multiarrayMediumLargeDeleteAtIdx(node, remaining, remainingLen,        \
                                         offsetLen, count, len)                \
    do {                                                                       \
        if (remaining) {                                                       \
            /* Move entries from (idx + 1) to (idx) */                         \
            memmove((node)->data + (offsetLen),                                \
                    (node)->data + (offsetLen) + (len), remainingLen);         \
        }                                                                      \
        (node)->data = zrealloc((node)->data, (len) * ((count)-1));            \
    } while (0)

#define multiarrayMediumLargeNodeNewAfter(nodeNew, nodeOld, remaining,         \
                                          remainingLen, offsetLen, s, len)     \
    do {                                                                       \
        /* If remaining is beyond the 50% mark, remaining will be smaller      \
         * than idx.                                                           \
         * Put new value at head then move remaining entries. */               \
        /* 'nodeNew' gets put AFTER 'nodeOld' */                               \
        (nodeNew)->data = zcalloc((len), ((remaining) + 1));                   \
        (nodeNew)->count = (remaining) + 1;                                    \
                                                                               \
        /* Append new entry */                                                 \
        memcpy((nodeNew)->data, s, len);                                       \
                                                                               \
        /* Copy 'remaining' (remaining->end) to 'nodeNew' */                   \
        memcpy((nodeNew)->data + (len), (nodeOld)->data + (offsetLen),         \
               remainingLen);                                                  \
                                                                               \
        /* Update nodeOld->data count */                                       \
        (nodeOld)->count -= (remaining);                                       \
                                                                               \
        /* Don't have to move remaining entries since we copied from end. */   \
        /* We can just realloc the size down to fit. */                        \
        (nodeOld)->data = zrealloc((nodeOld)->data, offsetLen);                \
    } while (0)

#define multiarrayMediumLargeNodeNew(nodeNew, nodeOld, offset, remainingLen,   \
                                     offsetLen, s, len)                        \
    do {                                                                       \
        /* else, remaining is *before* the 50% mark and remaining is           \
         * *bigger* than idx.                                                  \
         * Move 'idx' entires then place new value at TAIL. */                 \
        /* split gets put BEFORE found */                                      \
        split->data = zcalloc((len), ((offset) + 1));                          \
        split->count = (offset) + 1;                                           \
                                                                               \
        /* Copy (head->idx) 'split' */                                         \
        memcpy(split->data, found->data, offsetLen);                           \
                                                                               \
        /* Copy new element to 'split' */                                      \
        memcpy(split->data + (offsetLen), s, len);                             \
                                                                               \
        /* Update found->data count */                                         \
        found->count -= (offset);                                              \
                                                                               \
        /* Move 'found->data' entries down. */                                 \
        memmove(found->data, found->data + (offsetLen), remainingLen);         \
                                                                               \
        /* Shrink allocation since we removed entries */                       \
        found->data = zrealloc(found->data, remainingLen);                     \
    } while (0)
