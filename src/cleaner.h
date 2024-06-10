#pragma once
#include "datakit.h"

#define defcleaner(type, cleanupFunction)                                      \
    DK_FN_UNUSED void cleanupFunction##_cleanup(type *t) {                     \
        if (*t) {                                                              \
            cleanupFunction(*t);                                               \
        }                                                                      \
    }

/* Sometimes we need the same 'cleanupFunction' but with different type params,
 * so allow unlimited name differences even for the same 'cleanupFunction' */
#define defcleanerName(type, cleanupFunction, name)                            \
    DK_FN_UNUSED void cleanupFunction##name##_cleanup(type *t) {               \
        if (*t) {                                                              \
            cleanupFunction(*t);                                               \
        }                                                                      \
    }

#define cleaner(cleanupFunction)                                               \
    __attribute__((cleanup(cleanupFunction##_cleanup)))

#define cleanerName(cleanupFunction, name)                                     \
    __attribute__((cleanup(cleanupFunction##name##_cleanup)))
