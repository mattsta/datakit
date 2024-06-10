#pragma once

/* Use case:
 *   You want to use an array as an integer-indexed hash table, but your
 *   smallest index isn't zero.  Maybe your smallest index is 50 or 2000 and
 *   you don't want to allocate 2000 unused slots below where your data begins.
 *
 *   Solution: use an offsetArray to automatically adjust your indices down
 *   so your array starts at your smallest integer value and can still be
 *   addressed directly (with one subtraction to calculate the proper offset)
 *   and can still grow as necessary. */
#define offsetArrayCreateTypes(name, storage, scale)                           \
    typedef struct offsetArray##name {                                         \
        storage *obj;                                                          \
        scale offset;                                                          \
        scale highest;                                                         \
    } offsetArray##name

/* Internal helpers */
#define offsetArrayAdjusted_(arr, idx) ((idx) - (arr)->offset)
#define offsetArrayExpand_(arr, by)                                            \
    do {                                                                       \
        (arr)->obj = zrealloc(                                                 \
            (arr)->obj, (offsetArrayAdjusted_(arr, (arr)->highest) + (by)) *   \
                            sizeof(*(arr)->obj));                              \
    } while (0)

/* Allocate space so you can address 'idx' directly */
#define offsetArrayGrow(arr, idx)                                              \
    do {                                                                       \
        /* On init, we assume both are zero. */                                \
        if (!(arr)->highest && !(arr)->offset) {                               \
            (arr)->offset = (idx);                                             \
            (arr)->highest = (idx);                                            \
            offsetArrayExpand_(arr, 1);                                        \
        } else if ((idx) < (arr)->offset) {                                    \
            const size_t growBy = (arr)->offset - (idx);                       \
            offsetArrayExpand_(arr, growBy + 1);                               \
            memmove((arr)->obj + growBy, (arr)->obj,                           \
                    (1 + offsetArrayAdjusted_(arr, (arr)->highest)) *          \
                        sizeof(*(arr)->obj));                                  \
            (arr)->offset = (idx);                                             \
        } else if ((idx) > (arr)->highest) {                                   \
            offsetArrayExpand_(arr, offsetArrayAdjusted_(arr, idx) + 1);       \
            (arr)->highest = idx;                                              \
        }                                                                      \
    } while (0)

#define offsetArrayFree(arr)                                                   \
    do {                                                                       \
        zfree((arr)->obj);                                                     \
    } while (0)

/* Access 'idx' */
#define offsetArrayGet(arr, idx) ((arr)->obj[offsetArrayAdjusted_(arr, idx)])
#define offsetArrayCount(arr) ((arr)->highest - (arr)->offset + 1)
#define offsetArrayDirect(arr, zeroIdx) ((arr)->obj[zeroIdx])

#ifdef DATAKIT_TEST
int offsetArrayTest(int argc, char *argv[]);
#endif
