#pragma once

#include <stdatomic.h> /* C11 */

/* Shared Communal Data Structure Sharing */

/* The 'sharrow' interface only requires one struct member:
 *  'checkedOut'
 */

#define sharrowCount(s)                                                        \
    atomic_load_explicit(sharrowElement_(s), memory_order_relaxed)

#define sharrowRetain(s)                                                       \
    atomic_fetch_add_explicit(sharrowElement_(s), 1, memory_order_relaxed)

#define sharrowRetainCount(s, n)                                               \
    atomic_fetch_add_explicit(sharrowElement_(s), n, memory_order_relaxed)

/* If 'checkedOut' isn't a pointer (just a flat integer), run release check.
 * If 'checkedOut' IS a pointer, and NOT NULL, run the release check.
 * If 'checkedOut' IS a pointer, and NULL, run immediate release/free. */
/* Note: to "run release check" we return false here which DOESN'T
 *       short circut in sharrowRelease().
 *       to "run immediate" we return true which DOES short circuit. */
/* PURPOSE: prevents running atomics when (s)->checkedOut is NULL pointer;
 *          the result is inverted due to the OR condition in the final
 *          check, so here, "false" means GO AHEAD and "true" means STOP NOW. */
/* clang-format off */
#define sharrowReleaseQualify_(s)                     \
    _Generic((s)->checkedOut,                          \
                  uint32_t: false,                      \
                   int64_t: false,                       \
                  uint64_t: false,                        \
        _Atomic uint64_t *: (s)->checkedOut ? false : true )
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
/* Note: "_Atomic" is a property of pointers here, but NOT of scalar types.
 * _Atomic <int> is meaningless here, so just go without, but we *do* need
 * _Atomic for the pointer types. */
/* No default here because we get a better error message if no type
 * matches than if we fall back to a default value. */
/* clang-format off */
#define sharrowElement__(checkedOut) \
    _Generic((checkedOut),            \
                uint32_t: &checkedOut, \
                 int64_t: &checkedOut,  \
                uint64_t: &checkedOut,   \
        _Atomic uint64_t *: checkedOut    )
/* clang-format on */

#define sharrowReleaseCheckIsFinalOwner_(s) (sharrowRelease_(s) == 0)

#define sharrowReleaseCheckSafe_(s)

#define sharrowRelease_(s)                                                     \
    atomic_fetch_sub_explicit(sharrowElement_(s), 1, memory_order_relaxed)
