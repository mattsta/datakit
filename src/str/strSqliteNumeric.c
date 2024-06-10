#include "../str.h"

int64_t StrDoubleToInt64(double r) {
    static const int64_t maxInt = INT64_MAX;
    static const int64_t minInt = INT64_MIN;

    if (r <= (double)minInt) {
        return minInt;
    }

    if (r >= (double)maxInt) {
        return maxInt;
    }

    return (int64_t)r;
}

bool StrDoubleCanBeCastToInt64(const double r) {
    const int64_t ix = StrDoubleToInt64(r);
    /* Only mark the value as an integer if
    **
    **    (1) the round-trip conversion real->int->real is a no-op, and
    **    (2) The integer is neither the largest nor the smallest
    **        possible integer (ticket #3922)
    **
    ** The second and third terms in the following conditional enforces
    ** the second condition under the assumption that addition overflow causes
    ** values to wrap around.
    */
    if (r == ix && ix > INT64_MIN && ix < INT64_MAX) {
        return true;
    }

    return false;
}
