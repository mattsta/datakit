#pragma once
#include "datakit.h"

/* static: every translation unit that wants a cleaner defines its own
 * copy, so two files using defcleaner on the same function never
 * collide at link time. */
#define defcleaner(type, cleanupFunction)                                      \
    DK_FN_UNUSED static void cleanupFunction##_cleanup(type *t) {              \
        if (*t) {                                                              \
            cleanupFunction(*t);                                               \
        }                                                                      \
    }

/* Sometimes we need the same 'cleanupFunction' but with different type params,
 * so allow unlimited name differences even for the same 'cleanupFunction' */
#define defcleanerName(type, cleanupFunction, name)                            \
    DK_FN_UNUSED static void cleanupFunction##name##_cleanup(type *t) {        \
        if (*t) {                                                              \
            cleanupFunction(*t);                                               \
        }                                                                      \
    }

/* For a fixed array of 'n' owned pointers: `cleanerName(cleanupFunction,
 * name) type arr[n]` runs cleanupFunction on every non-null slot at scope
 * exit (null a slot to keep it) */
#define defcleanerArrayName(type, n, cleanupFunction, name)                    \
    DK_FN_UNUSED static void cleanupFunction##name##_cleanup(type(*a)[n]) {    \
        for (size_t i_ = 0; i_ < (n); i_++) {                                  \
            if ((*a)[i_]) {                                                    \
                cleanupFunction((*a)[i_]);                                     \
            }                                                                  \
        }                                                                      \
    }

#define cleaner(cleanupFunction) [[gnu::cleanup(cleanupFunction##_cleanup)]]

#define cleanerName(cleanupFunction, name)                                     \
    [[gnu::cleanup(cleanupFunction##name##_cleanup)]]

/* File-descriptor scope: `cleaner(fdClose) int fd = open(...);` closes
 * fd at scope exit when it is >= 0, so -1 (the failure value of every
 * descriptor-returning call) is the disarmed state.  errno is preserved
 * across the close, so a failure path can return straight after the
 * failing call and the caller still reads that call's errno.  Transfer
 * ownership with fdRelease(), which hands back the descriptor and
 * disarms the local. */
DK_FN_UNUSED static void fdClose_cleanup(int *fd) {
    if (*fd >= 0) {
        const int savedErrno = errno;
        close(*fd);
        errno = savedErrno;
    }
}

static inline int fdRelease(int *fd) {
    const int released = *fd;
    *fd = -1;
    return released;
}

/* Usage idioms:
 *
 * Scope-exit free (never leaks on any early return):
 *     defcleaner(void *, zfree);        // once per file, top level;
 *                                       // the trailing ';' is REQUIRED
 *     cleaner(zfree) void *buf = zmalloc(n);
 *
 * Ownership transfer ("disarm by NULLing"): when the success path
 * hands the allocation to someone else, NULL the local so the cleaner
 * (which skips NULL) does nothing:
 *     cleaner(zfree) void *out = zmalloc(n);
 *     ...
 *     *result = out;
 *     out = NULL;      // disarmed: caller owns it now
 *     return true;     // failure paths above still free automatically
 *
 * Descriptors use the same shape, with -1 as the disarmed value:
 *     cleaner(fdClose) int fd = open(path, O_RDONLY | O_CLOEXEC);
 *     if (fd < 0 || !setUp(fd)) {
 *         return -1;          // closed here if it was opened
 *     }
 *     return fdRelease(&fd);  // disarmed: caller owns it now
 *
 * goto interaction: a goto must never jump forward over a cleaner
 * declaration (clang rejects the jump, same class as VLA rules).
 * Hoist the declaration NULL-initialized above the first goto and
 * assign at the original site.
 *
 * Array of OWNED objects: elements cannot carry the attribute, so a
 * small holder struct owns the set — its cleanup frees each element,
 * then the array (see roarArray in multiroar.c; perfRegPtrArray in
 * perfRegression.c for the runtime-sized variant):
 *     typedef struct xArray { x **items; uint64_t count; } xArray;
 *     DK_FN_UNUSED static void xArrayFree_cleanup(xArray *a) { ... }
 *     cleaner(xArrayFree) xArray arr = {...};
 *
 * Wipe-then-free secret buffers: the cleanup fn only receives the
 * pointer, so a runtime wipe length rides in a {ptr, len} holder
 * whose cleanup wipes DCE-resistantly before freeing (see
 * hkrSecretBuf in hmacKeyring.c); disarm by zeroing the holder.
 *
 * State restore guard (undo a global tweak on every exit): make a
 * cleanup fn for a small guard type whose cleanup performs the
 * restore, e.g. restoring a forced backend selection. */
