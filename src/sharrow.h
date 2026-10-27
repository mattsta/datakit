#pragma once

#include <stdatomic.h> /* C11 */
#include <stdint.h>

/* Shared Communal Data Structure Sharing */

/* The 'sharrow' interface only requires one struct member:
 *  'checkedOut'
 *
 * checkedOut must be declared _Atomic: one of _Atomic uint32_t,
 * _Atomic int64_t, _Atomic uint64_t, or a pointer _Atomic uint64_t * to
 * a counter held elsewhere. Any other type, a plain (non-_Atomic)
 * integer included, is a compile error.
 *
 * The count is the number of owners beyond the first: a new object
 * starts at 0, each sharrowRetain adds one, and the release that finds
 * 0 is the final one and runs the releaser. Once the final release has
 * run, the counter's value is not meaningful (it has wrapped below
 * zero), which matters only for sharrowReleaseCustom, whose counter
 * lives in an object that outlives the release. */

/* Memory ordering.  The thread that drops the last reference runs the
 * releaser, which frees the block and everything it owns, so it must
 * observe every other owner's writes.  Each drop is therefore an
 * acq_rel decrement: the release half publishes the dropping owner's
 * accesses, and the acquire half lets the final drop (which reads from
 * the release sequence of all earlier drops) observe them.  An acq_rel
 * RMW is used rather than a relaxed decrement plus an acquire fence on
 * the zero branch because ThreadSanitizer does not model standalone
 * fences.  Retains stay relaxed: a retain is legal only from a thread
 * that already holds a reference. */

/* Observer only; not a synchronisation point. */
#define sharrowCount(s)                                                        \
    /* @sync: relaxed counter */                                               \
    atomic_load_explicit(sharrowElement_(s), memory_order_relaxed)

/* Relaxed: legal only from a thread that already holds a reference. */
#define sharrowRetain(s)                                                       \
    /* @sync: relaxed counter */                                               \
    atomic_fetch_add_explicit(sharrowElement_(s), 1, memory_order_relaxed)

#define sharrowRetainCount(s, n)                                               \
    /* @sync: relaxed counter */                                               \
    atomic_fetch_add_explicit(sharrowElement_(s), n, memory_order_relaxed)

/* If 'checkedOut' isn't a pointer (just a flat integer), run release check.
 * If 'checkedOut' IS a pointer, and NOT NULL, run the release check.
 * If 'checkedOut' IS a pointer, and NULL, run immediate release/free. */
/* Note: to "run release check" we return false here which DOESN'T
 *       short circuit in sharrowRelease().
 *       to "run immediate" we return true which DOES short circuit. */
/* PURPOSE: prevents running atomics when (s)->checkedOut is NULL pointer;
 *          the result is inverted due to the OR condition in the final
 *          check, so here, "false" means GO AHEAD and "true" means STOP NOW. */
/* Dispatches on the member's address, which keeps its qualifiers (see
 * sharrowElement__). */
/* clang-format off */
#define sharrowReleaseQualify_(s)                           \
    _Generic(&(s)->checkedOut,                               \
              _Atomic uint32_t *: false,                      \
               _Atomic int64_t *: false,                       \
              _Atomic uint64_t *: false,                        \
             _Atomic uint64_t **: (s)->checkedOut ? false : true )
/* clang-format on */

#define sharrowRelease(name, s)                                                \
    do {                                                                       \
        if (sharrowReleaseQualify_(s) ||                                       \
            sharrowReleaseCheckIsFinalOwner_(s)) {                             \
            sharrowReleaser##name(s);                                          \
        }                                                                      \
    } while (0)

/* Retained count is on something *not* 's', but we want to run our release
 * on 's' if retain count is zero inside 'what'. */
#define sharrowReleaseCustom(name, s, what)                                    \
    do {                                                                       \
        if (sharrowReleaseQualify_(what) ||                                    \
            sharrowReleaseCheckIsFinalOwner_(what)) {                          \
            sharrowReleaser##name(s);                                          \
        }                                                                      \
    } while (0)

#define sharrowElement_(s) sharrowElement__((s)->checkedOut)

/* Clang doesn't know this syntax, so it re-formats like a ternary. Avoid. */
/* Dispatch on the member's address rather than its value: lvalue
 * conversion of the value would strip _Atomic, so a plain integer member
 * would match and the atomics would then run on a non-atomic object.
 * The address keeps the qualifier, so only _Atomic members match. */
/* No default here because we get a better error message if no type
 * matches than if we fall back to a default value. */
/* clang-format off */
#define sharrowElement__(checkedOut)                \
    _Generic(&(checkedOut),                          \
              _Atomic uint32_t *: &(checkedOut),      \
               _Atomic int64_t *: &(checkedOut),       \
              _Atomic uint64_t *: &(checkedOut),        \
             _Atomic uint64_t **: (checkedOut)           )
/* clang-format on */

/* The final owner's acquire comes from the acq_rel decrement in
 * sharrowRelease_. */
#define sharrowReleaseCheckIsFinalOwner_(s) (sharrowRelease_(s) == 0)

#define sharrowReleaseCheckSafe_(s)

#define sharrowRelease_(s)                                                     \
    /* @sync: acq_rel sharrow.release */                                       \
    atomic_fetch_sub_explicit(sharrowElement_(s), 1, memory_order_acq_rel)
