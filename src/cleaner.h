#pragma once
#include "datakit.h"

/* static: every translation unit that wants a cleaner defines its own
 * copy, so two files using defcleaner on the same function never
 * collide at link time. */
#define defcleaner(type, cleanupFunction)                                      \
    static DK_FN_UNUSED void cleanupFunction##_cleanup(type *t) {              \
        if (*t) {                                                              \
            cleanupFunction(*t);                                               \
        }                                                                      \
    }

/* Sometimes we need the same 'cleanupFunction' but with different type params,
 * so allow unlimited name differences even for the same 'cleanupFunction' */
#define defcleanerName(type, cleanupFunction, name)                            \
    static DK_FN_UNUSED void cleanupFunction##name##_cleanup(type *t) {        \
        if (*t) {                                                              \
            cleanupFunction(*t);                                               \
        }                                                                      \
    }

#define cleaner(cleanupFunction)                                               \
    __attribute__((cleanup(cleanupFunction##_cleanup)))

#define cleanerName(cleanupFunction, name)                                     \
    __attribute__((cleanup(cleanupFunction##name##_cleanup)))

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
 *     static DK_FN_UNUSED void xArrayFree_cleanup(xArray *a) { ... }
 *     __attribute__((cleanup(xArrayFree_cleanup))) xArray arr = {...};
 *
 * Wipe-then-free secret buffers: the cleanup fn only receives the
 * pointer, so a runtime wipe length rides in a {ptr, len} holder
 * whose cleanup wipes DCE-resistantly before freeing (see
 * hkrSecretBuf in hmacKeyring.c); disarm by zeroing the holder.
 *
 * State restore guard (undo a global tweak on every exit): make a
 * cleanup fn for a small guard type whose cleanup performs the
 * restore, e.g. restoring a forced backend selection. */
