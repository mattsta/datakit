/**
 * This code is released under the
 * Apache License Version 2.0 http://www.apache.org/licenses/.
 *
 * Modified for language and style; originally from:
 * https://github.com/lemire/SIMDCompressionAndIntersection
 */

#include "intersectInt.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <pmmintrin.h>
#include <smmintrin.h>

#if USE_ALIGNED
#define MM_LOAD_SI_128 _mm_load_si128
#define MM_STORE_SI_128 _mm_store_si128
#else
#define MM_LOAD_SI_128 _mm_loadu_si128
// #define MM_STORE_SI_128 _mm_storeu_si128
#endif

/* clang-format off */
#define or ||
#define and &&
/* clang-format on */

/**
 * This is often called galloping or exponential search.
 */
static size_t frogadvanceUntil__(const uint32_t *array, const size_t pos,
                                 const size_t length, const size_t min) {
    size_t lower = pos + 1;

    // special handling for a possibly common sequential case
    if ((lower >= length) or (array[lower] >= min)) {
        return lower;
    }

    size_t spansize = 1; // could set larger
    // bootstrap an upper limit

    while ((lower + spansize < length) and (array[lower + spansize] < min)) {
        spansize *= 2;
    }

    size_t upper = (lower + spansize < length) ? lower + spansize : length - 1;

    if (array[upper] < min) { // means array has no item >= min
        return length;
    }

    // we know that the next-smallest span was too small
    lower += (spansize / 2);

    // else begin binary search
    size_t mid = 0;
    while (lower + 1 != upper) {
        mid = (lower + upper) / 2;
        if (array[mid] == min) {
            return mid;
        } else if (array[mid] < min) {
            lower = mid;
        } else {
            upper = mid;
        }
    }

    return upper;
}

size_t intersectIntOneSidedGalloping(const uint32_t *smallset,
                                     const size_t smalllength,
                                     const uint32_t *largeset,
                                     const size_t largelength, uint32_t *out) {
    if (largelength < smalllength) {
        return intersectIntOneSidedGalloping(largeset, largelength, smallset,
                                             smalllength, out);
    }

    if (0 == smalllength) {
        return 0;
    }

    const uint32_t *const initout = out;
    size_t k1 = 0;
    size_t k2 = 0;
    while (true) {
        if (largeset[k1] < smallset[k2]) {
            k1 = frogadvanceUntil__(largeset, k1, largelength, smallset[k2]);
            if (k1 == largelength) {
                break;
            }
        }

    midpoint:
        if (smallset[k2] < largeset[k1]) {
            ++k2;
            if (k2 == smalllength) {
                break;
            }
        } else {
            *out++ = smallset[k2];
            ++k2;
            if (k2 == smalllength) {
                break;
            }

            k1 = frogadvanceUntil__(largeset, k1, largelength, smallset[k2]);
            if (k1 == largelength) {
                break;
            }

            goto midpoint;
        }
    }

    return out - initout;
}

/**
 * Fast scalar scheme designed by N. Kurz.
 */
size_t scalar(const uint32_t *A, const size_t lenA, const uint32_t *B,
              const size_t lenB, uint32_t *out) {
    const uint32_t *const initout = out;
    if (lenA == 0 || lenB == 0) {
        return 0;
    }

    const uint32_t *endA = A + lenA;
    const uint32_t *endB = B + lenB;

    while (true) {
        while (*A < *B) {
        SKIP_FIRST_COMPARE:
            if (++A == endA) {
                return (out - initout);
            }
        }

        while (*A > *B) {
            if (++B == endB) {
                return (out - initout);
            }
        }

        if (*A == *B) {
            *out++ = *A;
            if (++A == endA || ++B == endB) {
                return (out - initout);
            }
        } else {
            goto SKIP_FIRST_COMPARE;
        }
    }

    __builtin_unreachable();
    return (out - initout); // NOTREACHED
}

size_t match_scalar(const uint32_t *A, const size_t lenA, const uint32_t *B,
                    const size_t lenB, uint32_t *out) {
    const uint32_t *initout = out;
    if (lenA == 0 || lenB == 0) {
        return 0;
    }

    const uint32_t *endA = A + lenA;
    const uint32_t *endB = B + lenB;

    while (true) {
        while (*A < *B) {
        SKIP_FIRST_COMPARE:
            if (++A == endA) {
                goto FINISH;
            }
        }

        while (*A > *B) {
            if (++B == endB) {
                goto FINISH;
            }
        }

        if (*A == *B) {
            *out++ = *A;
            if (++A == endA || ++B == endB) {
                goto FINISH;
            }
        } else {
            goto SKIP_FIRST_COMPARE;
        }
    }

FINISH:
    return (out - initout);
}

#ifdef __GNUC__
// #define COMPILER_LIKELY(x) __builtin_expect((x), 1)
#define COMPILER_RARELY(x) __builtin_expect((x), 0)
#else
#define COMPILER_LIKELY(x) x
#define COMPILER_RARELY(x) x
#endif

/**
 * Intersections scheme designed by N. Kurz that works very
 * well when intersecting an array with another where the density
 * differential is small (between 2 to 10).
 *
 * It assumes that lenRare <= lenFreq.
 *
 * Note that this is not symmetric: flipping the rare and freq pointers
 * as well as lenRare and lenFreq could lead to significant performance
 * differences.
 *
 * The matchOut pointer can safely be equal to the rare pointer.
 *
 */
size_t v1(const uint32_t *rare, size_t lenRare, const uint32_t *freq,
          size_t lenFreq, uint32_t *matchOut) {
    assert(lenRare <= lenFreq);
    const uint32_t *matchOrig = matchOut;
    if (lenFreq == 0 || lenRare == 0) {
        return 0;
    }

    const uint64_t kFreqSpace = 2 * 4 * (0 + 1) - 1;
    const uint64_t kRareSpace = 0;

    const uint32_t *stopFreq = &freq[lenFreq] - kFreqSpace;
    const uint32_t *stopRare = &rare[lenRare] - kRareSpace;

    if (COMPILER_RARELY((rare >= stopRare) || (freq >= stopFreq))) {
        goto FINISH_SCALAR;
    }

    uint32_t valRare = rare[0];
    uint64_t maxFreq = freq[2 * 4 - 1];

    __m128i Rare = _mm_set1_epi32(valRare);
    __m128i F0 = _mm_lddqu_si128((const __m128i *)(freq));
    __m128i F1 = _mm_lddqu_si128((const __m128i *)(freq + 4));

    if (COMPILER_RARELY(maxFreq < valRare)) {
        goto ADVANCE_FREQ;
    }

ADVANCE_RARE:
    do {
        *matchOut = valRare;
        rare += 1;
        if (COMPILER_RARELY(rare >= stopRare)) {
            rare -= 1;
            goto FINISH_SCALAR;
        }

        valRare = rare[0]; // for next iteration
        F0 = _mm_cmpeq_epi32(F0, Rare);
        F1 = _mm_cmpeq_epi32(F1, Rare);
        Rare = _mm_set1_epi32(valRare);
        F0 = _mm_or_si128(F0, F1);
#if __SSE4_1__
        if (_mm_testz_si128(F0, F0) == 0) {
            matchOut++;
        }

#else
        if (_mm_movemask_epi8(F0)) {
            matchOut++;
        }
#endif
        F0 = _mm_lddqu_si128((const __m128i *)(freq));
        F1 = _mm_lddqu_si128((const __m128i *)(freq + 4));

    } while (maxFreq >= valRare);

    uint64_t maxProbe;

ADVANCE_FREQ:
    do {
        const uint64_t kProbe = (0 + 1) * 2 * 4;
        const uint32_t *probeFreq = freq + kProbe;

        if (COMPILER_RARELY(probeFreq >= stopFreq)) {
            goto FINISH_SCALAR;
        }

        maxProbe = freq[(0 + 2) * 2 * 4 - 1];

        freq = probeFreq;

    } while (maxProbe < valRare);

    maxFreq = maxProbe;

    F0 = _mm_lddqu_si128((const __m128i *)(freq));
    F1 = _mm_lddqu_si128((const __m128i *)(freq + 4));

    goto ADVANCE_RARE;

    size_t count;
FINISH_SCALAR:
    count = matchOut - matchOrig;

    lenFreq = stopFreq + kFreqSpace - freq;
    lenRare = stopRare + kRareSpace - rare;

    size_t tail = match_scalar(freq, lenFreq, rare, lenRare, matchOut);

    return count + tail;
}

/**
 * This intersection function is similar to v1, but is faster when
 * the difference between lenRare and lenFreq is large, but not too large.

 * It assumes that lenRare <= lenFreq.
 *
 * Note that this is not symmetric: flipping the rare and freq pointers
 * as well as lenRare and lenFreq could lead to significant performance
 * differences.
 *
 * The matchOut pointer can safely be equal to the rare pointer.
 *
 * This function DOES NOT use inline assembly instructions. Just intrinsics.
 */
size_t v3(const uint32_t *rare, const size_t lenRare, const uint32_t *freq,
          const size_t lenFreq, uint32_t *out) {
    if (lenFreq == 0 || lenRare == 0) {
        return 0;
    }

    assert(lenRare <= lenFreq);
    const uint32_t *const initout = out;
    typedef __m128i vec;
    const uint32_t veclen = sizeof(vec) / sizeof(uint32_t);
    const size_t vecmax = veclen - 1;
    const size_t freqspace = 32 * veclen;
    const size_t rarespace = 1;

    const uint32_t *stopFreq = freq + lenFreq - freqspace;
    const uint32_t *stopRare = rare + lenRare - rarespace;
    if (freq > stopFreq) {
        return scalar(freq, lenFreq, rare, lenRare, out);
    }

    while (freq[veclen * 31 + vecmax] < *rare) {
        freq += veclen * 32;
        if (freq > stopFreq) {
            goto FINISH_SCALAR;
        }
    }

    for (; rare < stopRare; ++rare) {
        const uint32_t matchRare = *rare; // nextRare;
        const vec Match = _mm_set1_epi32(matchRare);
        while (freq[veclen * 31 + vecmax] < matchRare) { // if no match possible
            freq += veclen * 32;                         // advance 32 vectors
            if (freq > stopFreq) {
                goto FINISH_SCALAR;
            }
        }

        vec Q0, Q1, Q2, Q3;
        if (freq[veclen * 15 + vecmax] >= matchRare) {
            if (freq[veclen * 7 + vecmax] < matchRare) {
                Q0 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 8),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 9),
                                    Match));
                Q1 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 10),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 11),
                                    Match));

                Q2 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 12),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 13),
                                    Match));
                Q3 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 14),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 15),
                                    Match));
            } else {
                Q0 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 4),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 5),
                                    Match));
                Q1 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 6),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 7),
                                    Match));
                Q2 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 0),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 1),
                                    Match));
                Q3 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 2),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 3),
                                    Match));
            }
        } else {
            if (freq[veclen * 23 + vecmax] < matchRare) {
                Q0 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 8 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 9 + 16), Match));
                Q1 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 10 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 11 + 16), Match));

                Q2 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 12 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 13 + 16), Match));
                Q3 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 14 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 15 + 16), Match));
            } else {
                Q0 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 4 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 5 + 16), Match));
                Q1 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 6 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 7 + 16), Match));
                Q2 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 0 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 1 + 16), Match));
                Q3 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 2 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 3 + 16), Match));
            }
        }

        const vec F0 = _mm_or_si128(_mm_or_si128(Q0, Q1), _mm_or_si128(Q2, Q3));
#if __SSE4_1__
        if (_mm_testz_si128(F0, F0)) {
#else
        if (!_mm_movemask_epi8(F0)) {
#endif
        } else {
            *out++ = matchRare;
        }
    }

FINISH_SCALAR:
    return (out - initout) + scalar(freq, stopFreq + freqspace - freq, rare,
                                    stopRare + rarespace - rare, out);
}

/**
 * This is the SIMD galloping function. This intersection function works well
 * when lenRare and lenFreq have vastly different values.
 *
 * It assumes that lenRare <= lenFreq.
 *
 * Note that this is not symmetric: flipping the rare and freq pointers
 * as well as lenRare and lenFreq could lead to significant performance
 * differences.
 *
 * The matchOut pointer can safely be equal to the rare pointer.
 *
 * This function DOES NOT use assembly. It only relies on intrinsics.
 */
size_t SIMDgalloping(const uint32_t *rare, const size_t lenRare,
                     const uint32_t *freq, const size_t lenFreq,
                     uint32_t *out) {
    if (lenFreq == 0 || lenRare == 0) {
        return 0;
    }

    assert(lenRare <= lenFreq);
    const uint32_t *const initout = out;
    typedef __m128i vec;
    const uint32_t veclen = sizeof(vec) / sizeof(uint32_t);
    const size_t vecmax = veclen - 1;
    const size_t freqspace = 32 * veclen;
    const size_t rarespace = 1;

    const uint32_t *stopFreq = freq + lenFreq - freqspace;
    const uint32_t *stopRare = rare + lenRare - rarespace;
    if (freq > stopFreq) {
        return scalar(freq, lenFreq, rare, lenRare, out);
    }

    for (; rare < stopRare; ++rare) {
        const uint32_t matchRare = *rare; // nextRare;
        const vec Match = _mm_set1_epi32(matchRare);

        if (freq[veclen * 31 + vecmax] < matchRare) { // if no match possible
            uint32_t offset = 1;
            if (freq + veclen * 32 > stopFreq) {
                freq += veclen * 32;
                goto FINISH_SCALAR;
            }

            while (freq[veclen * offset * 32 + veclen * 31 + vecmax] <
                   matchRare) { // if no match possible
                if (freq + veclen * (2 * offset) * 32 <= stopFreq) {
                    offset *= 2;
                } else if (freq + veclen * (offset + 1) * 32 <= stopFreq) {
                    offset = (uint32_t)((stopFreq - freq) / (veclen * 32));
                    // offset += 1;
                    if (freq[veclen * offset * 32 + veclen * 31 + vecmax] <
                        matchRare) {
                        freq += veclen * offset * 32;
                        goto FINISH_SCALAR;
                    } else {
                        break;
                    }
                } else {
                    freq += veclen * offset * 32;
                    goto FINISH_SCALAR;
                }
            }

            uint32_t lower = offset / 2;
            while (lower + 1 != offset) {
                const uint32_t mid = (lower + offset) / 2;
                if (freq[veclen * mid * 32 + veclen * 31 + vecmax] <
                    matchRare) {
                    lower = mid;
                } else {
                    offset = mid;
                }
            }

            freq += veclen * offset * 32;
        }

        vec Q0, Q1, Q2, Q3;
        if (freq[veclen * 15 + vecmax] >= matchRare) {
            if (freq[veclen * 7 + vecmax] < matchRare) {
                Q0 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 8),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 9),
                                    Match));
                Q1 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 10),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 11),
                                    Match));

                Q2 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 12),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 13),
                                    Match));
                Q3 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 14),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 15),
                                    Match));
            } else {
                Q0 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 4),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 5),
                                    Match));
                Q1 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 6),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 7),
                                    Match));
                Q2 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 0),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 1),
                                    Match));
                Q3 = _mm_or_si128(
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 2),
                                    Match),
                    _mm_cmpeq_epi32(_mm_loadu_si128((const vec *)(freq) + 3),
                                    Match));
            }
        } else {
            if (freq[veclen * 23 + vecmax] < matchRare) {
                Q0 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 8 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 9 + 16), Match));
                Q1 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 10 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 11 + 16), Match));

                Q2 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 12 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 13 + 16), Match));
                Q3 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 14 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 15 + 16), Match));
            } else {
                Q0 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 4 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 5 + 16), Match));
                Q1 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 6 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 7 + 16), Match));
                Q2 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 0 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 1 + 16), Match));
                Q3 = _mm_or_si128(
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 2 + 16), Match),
                    _mm_cmpeq_epi32(
                        _mm_loadu_si128((const vec *)(freq) + 3 + 16), Match));
            }
        }

        const vec F0 = _mm_or_si128(_mm_or_si128(Q0, Q1), _mm_or_si128(Q2, Q3));
#if __SSE4_1__
        if (_mm_testz_si128(F0, F0)) {
#else
        if (!_mm_movemask_epi8(F0)) {
#endif
        } else {
            *out++ = matchRare;
        }
    }

FINISH_SCALAR:
    return (out - initout) + scalar(freq, stopFreq + freqspace - freq, rare,
                                    stopRare + rarespace - rare, out);
}

/**
 * Our main heuristic.
 *
 * The out pointer can be set1 if length1<=length2,
 * or else it can be set2 if length1>length2.
 */
size_t intersectInt(const uint32_t *set1, const size_t length1,
                    const uint32_t *set2, const size_t length2, uint32_t *out) {
    if ((length1 == 0) or (length2 == 0)) {
        return 0;
    }

    if ((1000 * length1 <= length2) or (1000 * length2 <= length1)) {
        if (length1 <= length2) {
            return SIMDgalloping(set1, length1, set2, length2, out);
        }

        return SIMDgalloping(set2, length2, set1, length1, out);
    }

    if ((50 * length1 <= length2) or (50 * length2 <= length1)) {
        if (length1 <= length2) {
            return v3(set1, length1, set2, length2, out);
        }

        return v3(set2, length2, set1, length1, out);
    }

    if (length1 <= length2) {
        return v1(set1, length1, set2, length2, out);
    }

    return v1(set2, length2, set1, length1, out);
}

#if __AVX2__
size_t v1_avx2(const uint32_t *rare, size_t lenRare, const uint32_t *freq,
               size_t lenFreq, uint32_t *matchOut) {
    assert(lenRare <= lenFreq);
    if (lenFreq == 0 || lenRare == 0) {
        return 0;
    }

    const uint32_t *matchOrig = matchOut;
    const uint64_t kFreqSpace = 2 * 4 * (0 + 1) - 1;
    const uint64_t kRareSpace = 0;

    const uint32_t *stopFreq = &freq[lenFreq] - kFreqSpace;
    const uint32_t *stopRare = &rare[lenRare] - kRareSpace;

#if 0
    printf("StopFreq: %p, Freq: %p, END: %p\n", stopFreq, freq, freq + lenFreq);
    printf("StopRare: %p, Rare: %p, END: %p\n", stopRare, rare, rare + lenRare);
#endif

    if (COMPILER_RARELY((rare >= stopRare) || (freq >= stopFreq))) {
        goto FINISH_SCALAR;
    }

    uint32_t valRare = rare[0];
    uint64_t maxFreq = freq[2 * 4 - 1];
    __m256i Rare = _mm256_set1_epi32(valRare);
    __m256i F = _mm256_loadu_si256((const __m256i *)(freq));

    if (COMPILER_RARELY(maxFreq < valRare)) {
        goto ADVANCE_FREQ;
    }

ADVANCE_RARE:
    do {
        *matchOut = valRare;

        /* TODO: refactor all this offset math to use more logical logic than
         * this poor 'stopRare' and 'stopFreq' measurement. Make sure we don't
         * read ahead of the stop condition. */
        if (COMPILER_RARELY((rare + 1) >= stopRare)) {
            goto FINISH_SCALAR;
        }

        valRare = rare[1]; // for next iteration
        rare += 1;

        F = _mm256_cmpeq_epi32(F, Rare);
        Rare = _mm256_set1_epi32(valRare);
        if (_mm256_testz_si256(F, F) == 0) {
            matchOut++;
        }

        F = _mm256_loadu_si256((const __m256i *)(freq));
    } while (maxFreq >= valRare);

    uint64_t maxProbe;

ADVANCE_FREQ:
    do {
        const uint64_t kProbe = (0 + 1) * 2 * 4;
        const uint32_t *probeFreq = freq + kProbe;

        /* Check compiler condition before reading memory beyond our expected
         * extent. */
        if (COMPILER_RARELY(probeFreq >= stopFreq)) {
            goto FINISH_SCALAR;
        }

        maxProbe = freq[(0 + 2) * 2 * 4 - 1];
        freq = probeFreq;
    } while (maxProbe < valRare);

    maxFreq = maxProbe;

    F = _mm256_loadu_si256((const __m256i *)(freq));

    goto ADVANCE_RARE;

    size_t count;
FINISH_SCALAR:
    count = matchOut - matchOrig;

    lenFreq = stopFreq + kFreqSpace - freq;
    lenRare = stopRare + kRareSpace - rare;

    size_t tail = match_scalar(freq, lenFreq, rare, lenRare, matchOut);

    return count + tail;
}

size_t v3_avx2(const uint32_t *rare, const size_t lenRare, const uint32_t *freq,
               const size_t lenFreq, uint32_t *out) {
    if (lenFreq == 0 || lenRare == 0) {
        return 0;
    }

    assert(lenRare <= lenFreq);
    const uint32_t *const initout = out;
    typedef __m256i vec;
    const uint32_t veclen = sizeof(vec) / sizeof(uint32_t);
    const size_t vecmax = veclen - 1;
    const size_t freqspace = 32 * veclen;
    const size_t rarespace = 1;

    const uint32_t *stopFreq = freq + lenFreq - freqspace;
    const uint32_t *stopRare = rare + lenRare - rarespace;
    if (freq > stopFreq) {
        return scalar(freq, lenFreq, rare, lenRare, out);
    }

    while (freq[veclen * 31 + vecmax] < *rare) {
        freq += veclen * 32;
        if (freq > stopFreq) {
            goto FINISH_SCALAR;
        }
    }

    for (; rare < stopRare; ++rare) {
        const uint32_t matchRare = *rare; // nextRare;
        const vec Match = _mm256_set1_epi32(matchRare);
        while (freq[veclen * 31 + vecmax] < matchRare) { // if no match possible
            freq += veclen * 32;                         // advance 32 vectors
            if (freq > stopFreq) {
                goto FINISH_SCALAR;
            }
        }

        vec Q0, Q1, Q2, Q3;
        if (freq[veclen * 15 + vecmax] >= matchRare) {
            if (freq[veclen * 7 + vecmax] < matchRare) {
                Q0 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 8),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 9),
                                       Match));
                Q1 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 10),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 11),
                                       Match));

                Q2 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 12),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 13),
                                       Match));
                Q3 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 14),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 15),
                                       Match));
            } else {
                Q0 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 4),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 5),
                                       Match));
                Q1 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 6),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 7),
                                       Match));
                Q2 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 0),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 1),
                                       Match));
                Q3 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 2),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 3),
                                       Match));
            }
        } else {
            if (freq[veclen * 23 + vecmax] < matchRare) {
                Q0 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 8 + 16),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 9 + 16),
                                       Match));
                Q1 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(
                        _mm256_loadu_si256((vec *)freq + 10 + 16), Match),
                    _mm256_cmpeq_epi32(
                        _mm256_loadu_si256((vec *)freq + 11 + 16), Match));

                Q2 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(
                        _mm256_loadu_si256((vec *)freq + 12 + 16), Match),
                    _mm256_cmpeq_epi32(
                        _mm256_loadu_si256((vec *)freq + 13 + 16), Match));
                Q3 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(
                        _mm256_loadu_si256((vec *)freq + 14 + 16), Match),
                    _mm256_cmpeq_epi32(
                        _mm256_loadu_si256((vec *)freq + 15 + 16), Match));
            } else {
                Q0 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 4 + 16),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 5 + 16),
                                       Match));
                Q1 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 6 + 16),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 7 + 16),
                                       Match));
                Q2 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 0 + 16),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 1 + 16),
                                       Match));
                Q3 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 2 + 16),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 3 + 16),
                                       Match));
            }
        }

        const vec F0 =
            _mm256_or_si256(_mm256_or_si256(Q0, Q1), _mm256_or_si256(Q2, Q3));
        if (_mm256_testz_si256(F0, F0)) {
        } else {
            *out++ = matchRare;
        }
    }

FINISH_SCALAR:
    return (out - initout) + scalar(freq, stopFreq + freqspace - freq, rare,
                                    stopRare + rarespace - rare, out);
}

size_t SIMDgalloping_avx2(const uint32_t *rare, const size_t lenRare,
                          const uint32_t *freq, const size_t lenFreq,
                          uint32_t *out) {
    if (lenFreq == 0 || lenRare == 0) {
        return 0;
    }

    assert(lenRare <= lenFreq);
    const uint32_t *const initout = out;
    typedef __m256i vec;
    const uint32_t veclen = sizeof(vec) / sizeof(uint32_t);
    const size_t vecmax = veclen - 1;
    const size_t freqspace = 32 * veclen;
    const size_t rarespace = 1;

    const uint32_t *stopFreq = freq + lenFreq - freqspace;
    const uint32_t *stopRare = rare + lenRare - rarespace;
    if (freq > stopFreq) {
        return scalar(freq, lenFreq, rare, lenRare, out);
    }

    for (; rare < stopRare; ++rare) {
        const uint32_t matchRare = *rare; // nextRare;
        const vec Match = _mm256_set1_epi32(matchRare);

        if (freq[veclen * 31 + vecmax] < matchRare) { // if no match possible
            uint32_t offset = 1;
            if (freq + veclen * 32 > stopFreq) {
                freq += veclen * 32;
                goto FINISH_SCALAR;
            }

            while (freq[veclen * offset * 32 + veclen * 31 + vecmax] <
                   matchRare) { // if no match possible
                if (freq + veclen * (2 * offset) * 32 <= stopFreq) {
                    offset *= 2;
                } else if (freq + veclen * (offset + 1) * 32 <= stopFreq) {
                    offset = (uint32_t)((stopFreq - freq) / (veclen * 32));
                    // offset += 1;
                    if (freq[veclen * offset * 32 + veclen * 31 + vecmax] <
                        matchRare) {
                        freq += veclen * offset * 32;
                        goto FINISH_SCALAR;
                    } else {
                        break;
                    }
                } else {
                    freq += veclen * offset * 32;
                    goto FINISH_SCALAR;
                }
            }

            uint32_t lower = offset / 2;
            while (lower + 1 != offset) {
                const uint32_t mid = (lower + offset) / 2;
                if (freq[veclen * mid * 32 + veclen * 31 + vecmax] <
                    matchRare) {
                    lower = mid;
                } else {
                    offset = mid;
                }
            }

            freq += veclen * offset * 32;
        }

        vec Q0, Q1, Q2, Q3;
        if (freq[veclen * 15 + vecmax] >= matchRare) {
            if (freq[veclen * 7 + vecmax] < matchRare) {
                Q0 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 8),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 9),
                                       Match));
                Q1 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 10),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 11),
                                       Match));

                Q2 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 12),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 13),
                                       Match));
                Q3 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 14),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 15),
                                       Match));
            } else {
                Q0 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 4),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 5),
                                       Match));
                Q1 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 6),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 7),
                                       Match));
                Q2 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 0),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 1),
                                       Match));
                Q3 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 2),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 3),
                                       Match));
            }
        } else {
            if (freq[veclen * 23 + vecmax] < matchRare) {
                Q0 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 8 + 16),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 9 + 16),
                                       Match));
                Q1 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(
                        _mm256_loadu_si256((vec *)freq + 10 + 16), Match),
                    _mm256_cmpeq_epi32(
                        _mm256_loadu_si256((vec *)freq + 11 + 16), Match));

                Q2 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(
                        _mm256_loadu_si256((vec *)freq + 12 + 16), Match),
                    _mm256_cmpeq_epi32(
                        _mm256_loadu_si256((vec *)freq + 13 + 16), Match));
                Q3 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(
                        _mm256_loadu_si256((vec *)freq + 14 + 16), Match),
                    _mm256_cmpeq_epi32(
                        _mm256_loadu_si256((vec *)freq + 15 + 16), Match));
            } else {
                Q0 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 4 + 16),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 5 + 16),
                                       Match));
                Q1 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 6 + 16),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 7 + 16),
                                       Match));
                Q2 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 0 + 16),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 1 + 16),
                                       Match));
                Q3 = _mm256_or_si256(
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 2 + 16),
                                       Match),
                    _mm256_cmpeq_epi32(_mm256_loadu_si256((vec *)freq + 3 + 16),
                                       Match));
            }
        }

        const vec F0 =
            _mm256_or_si256(_mm256_or_si256(Q0, Q1), _mm256_or_si256(Q2, Q3));
        if (_mm256_testz_si256(F0, F0)) {
        } else {
            *out++ = matchRare;
        }
    }

FINISH_SCALAR:
    return (out - initout) + scalar(freq, stopFreq + freqspace - freq, rare,
                                    stopRare + rarespace - rare, out);
}

/**
 * Our main heuristic.
 *
 * The out pointer can be set1 if length1<=length2,
 * or else it can be set2 if length1>length2.
 */
size_t intersectIntAVX2(const uint32_t *set1, const size_t length1,
                        const uint32_t *set2, const size_t length2,
                        uint32_t *out) {
    if ((length1 == 0) or (length2 == 0)) {
        return 0;
    }

    if ((1000 * length1 <= length2) or (1000 * length2 <= length1)) {
        if (length1 <= length2) {
            return SIMDgalloping_avx2(set1, length1, set2, length2, out);
        }

        return SIMDgalloping_avx2(set2, length2, set1, length1, out);
    }

    if ((50 * length1 <= length2) or (50 * length2 <= length1)) {
        if (length1 <= length2) {
            return v3_avx2(set1, length1, set2, length2, out);
        }

        return v3_avx2(set2, length2, set1, length1, out);
    }

    if (length1 <= length2) {
        return v1_avx2(set1, length1, set2, length2, out);
    }

    return v1_avx2(set2, length2, set1, length1, out);
}

#endif

/**
 * Shuffle mask table for compacting matching elements to the front of SSE register.
 * More or less from http://highlyscalable.wordpress.com/2012/06/05/fast-intersection-sorted-lists-sse/
 *
 * For a 4-bit mask where bit i indicates element i matched, this table provides
 * the _mm_shuffle_epi8 pattern to move matching elements to positions 0,1,2,...
 *
 * Element layout in 128-bit register: elem0=bytes[0-3], elem1=bytes[4-7],
 * elem2=bytes[8-11], elem3=bytes[12-15]. Unused positions filled with 0x80 (zeros output).
 */
/* clang-format off */
static const uint8_t shuffle_mask_src[16 * 16] = {
    /* mask 0 (0000): no matches */
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    /* mask 1 (0001): elem 0 */
    0, 1, 2, 3, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    /* mask 2 (0010): elem 1 */
    4, 5, 6, 7, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    /* mask 3 (0011): elem 0, 1 */
    0, 1, 2, 3, 4, 5, 6, 7, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    /* mask 4 (0100): elem 2 */
    8, 9, 10, 11, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    /* mask 5 (0101): elem 0, 2 */
    0, 1, 2, 3, 8, 9, 10, 11, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    /* mask 6 (0110): elem 1, 2 */
    4, 5, 6, 7, 8, 9, 10, 11, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    /* mask 7 (0111): elem 0, 1, 2 */
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x80, 0x80, 0x80, 0x80,
    /* mask 8 (1000): elem 3 */
    12, 13, 14, 15, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    /* mask 9 (1001): elem 0, 3 */
    0, 1, 2, 3, 12, 13, 14, 15, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    /* mask 10 (1010): elem 1, 3 */
    4, 5, 6, 7, 12, 13, 14, 15, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    /* mask 11 (1011): elem 0, 1, 3 */
    0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15, 0x80, 0x80, 0x80, 0x80,
    /* mask 12 (1100): elem 2, 3 */
    8, 9, 10, 11, 12, 13, 14, 15, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    /* mask 13 (1101): elem 0, 2, 3 */
    0, 1, 2, 3, 8, 9, 10, 11, 12, 13, 14, 15, 0x80, 0x80, 0x80, 0x80,
    /* mask 14 (1110): elem 1, 2, 3 */
    4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0x80, 0x80, 0x80, 0x80,
    /* mask 15 (1111): all elements */
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};
/* clang-format on */

const static __m128i *shuffle_mask = (__m128i *)&shuffle_mask_src;

// precomputed dictionary

/**
 * It is not safe for out to be either A or B.
 */
size_t highlyscalable_intersect_SIMD(const uint32_t *A, const size_t s_a,
                                     const uint32_t *B, const size_t s_b,
                                     uint32_t *out) {
    assert(out != A);
    assert(out != B);
    const uint32_t *const initout = out;
    size_t i_a = 0;
    size_t i_b = 0;

    // trim lengths to be a multiple of 4
    size_t st_a = (s_a / 4) * 4;
    size_t st_b = (s_b / 4) * 4;

    while (i_a < st_a && i_b < st_b) {
        //[ load segments of four 32-bit elements
        __m128i v_a = _mm_loadu_si128((__m128i *)&A[i_a]);
        __m128i v_b = _mm_loadu_si128((__m128i *)&B[i_b]);
        //]

        //[ move pointers
        const uint32_t a_max = A[i_a + 3];
        const uint32_t b_max = B[i_b + 3];
        i_a += (a_max <= b_max) * 4;
        i_b += (a_max >= b_max) * 4;
        //]

        //[ compute mask of common elements
#define cyclic_shift _MM_SHUFFLE(0, 3, 2, 1)
        __m128i cmp_mask1 = _mm_cmpeq_epi32(v_a, v_b); // pairwise comparison
        v_b = _mm_shuffle_epi32(v_b, cyclic_shift);    // shuffling
        __m128i cmp_mask2 = _mm_cmpeq_epi32(v_a, v_b); // again...
        v_b = _mm_shuffle_epi32(v_b, cyclic_shift);
        __m128i cmp_mask3 = _mm_cmpeq_epi32(v_a, v_b); // and again...
        v_b = _mm_shuffle_epi32(v_b, cyclic_shift);
        __m128i cmp_mask4 = _mm_cmpeq_epi32(v_a, v_b); // and again.
        __m128i cmp_mask = _mm_or_si128(
            _mm_or_si128(cmp_mask1, cmp_mask2),
            _mm_or_si128(cmp_mask3, cmp_mask4)); // OR-ing of comparison masks
        // convert the 128-bit mask to the 4-bit mask
        const int mask = _mm_movemask_ps(_mm_castsi128_ps(cmp_mask));
        //]

        //[ copy out common elements
        const __m128i p = _mm_shuffle_epi8(v_a, shuffle_mask[mask]);
        _mm_storeu_si128((__m128i *)out, p);
        out += __builtin_popcount(mask);
        // a number of elements is a weight of the mask ]
    }

    // intersect the tail using scalar intersection
    while (i_a < s_a && i_b < s_b) {
        if (A[i_a] < B[i_b]) {
            i_a++;
        } else if (B[i_b] < A[i_a]) {
            i_b++;
        } else {
            *out++ = B[i_b];
            i_a++;
            i_b++;
        }
    }

    return out - initout;
}

/**
 * Version optimized by D. Lemire
 *
 * It is not safe for out to be either A or B.
 */
size_t lemire_highlyscalable_intersect_SIMD(const uint32_t *A, const size_t s_a,
                                            const uint32_t *B, const size_t s_b,
                                            uint32_t *out) {
    assert(out != A);
    assert(out != B);
    const uint32_t *const initout = out;
    size_t i_a = 0;
    size_t i_b = 0;
#define cyclic_shift1 _MM_SHUFFLE(0, 3, 2, 1)
#define cyclic_shift2 _MM_SHUFFLE(1, 0, 3, 2)
#define cyclic_shift3 _MM_SHUFFLE(2, 1, 0, 3)

    // trim lengths to be a multiple of 4
    size_t st_a = (s_a / 4) * 4;
    size_t st_b = (s_b / 4) * 4;
    if (i_a < st_a && i_b < st_b) {
        __m128i v_a, v_b;
        v_a = MM_LOAD_SI_128((__m128i *)&A[i_a]);
        v_b = MM_LOAD_SI_128((__m128i *)&B[i_b]);
        while (true) {
            const __m128i cmp_mask1 =
                _mm_cmpeq_epi32(v_a, v_b); // pairwise comparison
            const __m128i cmp_mask2 = _mm_cmpeq_epi32(
                v_a, _mm_shuffle_epi32(v_b, cyclic_shift1)); // again...
            __m128i cmp_mask = _mm_or_si128(cmp_mask1, cmp_mask2);
            const __m128i cmp_mask3 = _mm_cmpeq_epi32(
                v_a, _mm_shuffle_epi32(v_b, cyclic_shift2)); // and again...
            cmp_mask = _mm_or_si128(cmp_mask, cmp_mask3);
            const __m128i cmp_mask4 = _mm_cmpeq_epi32(
                v_a, _mm_shuffle_epi32(v_b, cyclic_shift3)); // and again.
            cmp_mask = _mm_or_si128(cmp_mask, cmp_mask4);
            // convert the 128-bit mask to the 4-bit mask
            const int mask = _mm_movemask_ps(*(__m128 *)(&cmp_mask));
            // copy out common elements
            const __m128i p = _mm_shuffle_epi8(v_a, shuffle_mask[mask]);

            _mm_storeu_si128((__m128i *)out, p);

            // a number of elements is a weight of the mask
            out += __builtin_popcount(mask);

            const uint32_t a_max = A[i_a + 3];
            if (a_max <= B[i_b + 3]) {
                i_a += 4;
                if (i_a >= st_a) {
                    break;
                }

                v_a = MM_LOAD_SI_128((__m128i *)&A[i_a]);
            }

            if (a_max >= B[i_b + 3]) {
                i_b += 4;
                if (i_b >= st_b) {
                    break;
                }

                v_b = MM_LOAD_SI_128((__m128i *)&B[i_b]);
            }
        }
    }

    // intersect the tail using scalar intersection
    while (i_a < s_a && i_b < s_b) {
        if (A[i_a] < B[i_b]) {
            i_a++;
        } else if (B[i_b] < A[i_a]) {
            i_b++;
        } else {
            *out++ = B[i_b];
            i_a++;
            i_b++;
        }
    }

    return out - initout;
}

#ifdef DATAKIT_TEST
#include "ctest.h"

#include <inttypes.h> /* PRIu64, PRIu32 */
#include <string.h>   /* memset */

/* ====================================================================
 * Helper functions for tests
 * ==================================================================== */
typedef size_t (*intersectFn)(const uint32_t *, size_t, const uint32_t *, size_t,
                              uint32_t *);

/* Test that intersection of two arrays produces expected result */
static bool testIntersection(intersectFn fn, const char *fnName,
                             const uint32_t *a, size_t lenA, const uint32_t *b,
                             size_t lenB, const uint32_t *expected,
                             size_t expectedLen) {
    /* Output buffer - max possible size is min of both arrays */
    uint32_t out[1024];
    memset(out, 0xFF, sizeof(out));

    size_t result = fn(a, lenA, b, lenB, out);

    if (result != expectedLen) {
        printf("[%s] FAIL: Expected length %" PRIu64 ", got %" PRIu64 "\n",
               fnName, (uint64_t)expectedLen, (uint64_t)result);
        return false;
    }

    for (size_t i = 0; i < expectedLen; i++) {
        if (out[i] != expected[i]) {
            printf("[%s] FAIL: at index %" PRIu64 ", expected %" PRIu32
                   ", got %" PRIu32 "\n",
                   fnName, (uint64_t)i, expected[i], out[i]);
            return false;
        }
    }

    return true;
}

/* Test empty arrays */
static int32_t testEmptyArrays(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    /* Both empty */
    uint32_t out[16];
    size_t result = fn(NULL, 0, NULL, 0, out);
    if (result != 0) {
        ERR("[%s] Both empty should return 0, got %" PRIu64, fnName,
            (uint64_t)result);
    }

    /* First empty */
    uint32_t b[] = {1, 2, 3, 4};
    result = fn(NULL, 0, b, 4, out);
    if (result != 0) {
        ERR("[%s] First empty should return 0, got %" PRIu64, fnName,
            (uint64_t)result);
    }

    /* Second empty */
    uint32_t a[] = {1, 2, 3, 4};
    result = fn(a, 4, NULL, 0, out);
    if (result != 0) {
        ERR("[%s] Second empty should return 0, got %" PRIu64, fnName,
            (uint64_t)result);
    }

    return err;
}

/* Test single element arrays */
static int32_t testSingleElements(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    /* Single element, match */
    uint32_t a1[] = {42};
    uint32_t b1[] = {42};
    uint32_t exp1[] = {42};
    if (!testIntersection(fn, fnName, a1, 1, b1, 1, exp1, 1)) {
        ERR("[%s] Single match failed", fnName);
    }

    /* Single element, no match */
    uint32_t a2[] = {10};
    uint32_t b2[] = {20};
    if (!testIntersection(fn, fnName, a2, 1, b2, 1, NULL, 0)) {
        ERR("[%s] Single no-match failed", fnName);
    }

    /* Single vs multiple, match */
    uint32_t a3[] = {5};
    uint32_t b3[] = {1, 3, 5, 7, 9};
    uint32_t exp3[] = {5};
    if (!testIntersection(fn, fnName, a3, 1, b3, 5, exp3, 1)) {
        ERR("[%s] Single vs multiple match failed", fnName);
    }

    /* Single vs multiple, no match */
    uint32_t a4[] = {6};
    uint32_t b4[] = {1, 3, 5, 7, 9};
    if (!testIntersection(fn, fnName, a4, 1, b4, 5, NULL, 0)) {
        ERR("[%s] Single vs multiple no-match failed", fnName);
    }

    return err;
}

/* Test disjoint sets (no common elements) */
static int32_t testDisjoint(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    /* Interleaved disjoint */
    uint32_t a[] = {2, 4, 6, 8, 10};
    uint32_t b[] = {1, 3, 5, 7, 9};
    if (!testIntersection(fn, fnName, a, 5, b, 5, NULL, 0)) {
        ERR("[%s] Interleaved disjoint failed", fnName);
    }

    /* Non-overlapping ranges */
    uint32_t a2[] = {1, 2, 3, 4, 5};
    uint32_t b2[] = {10, 20, 30, 40, 50};
    if (!testIntersection(fn, fnName, a2, 5, b2, 5, NULL, 0)) {
        ERR("[%s] Non-overlapping ranges failed", fnName);
    }

    return err;
}

/* Test identical sets */
static int32_t testIdentical(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    uint32_t a[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t b[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t expected[] = {1, 2, 3, 4, 5, 6, 7, 8};
    if (!testIntersection(fn, fnName, a, 8, b, 8, expected, 8)) {
        ERR("[%s] Identical sets failed", fnName);
    }

    return err;
}

/* Test partial overlap */
static int32_t testPartialOverlap(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    /* Beginning overlap */
    uint32_t a1[] = {1, 2, 3, 4, 5};
    uint32_t b1[] = {1, 2, 3, 10, 20};
    uint32_t exp1[] = {1, 2, 3};
    if (!testIntersection(fn, fnName, a1, 5, b1, 5, exp1, 3)) {
        ERR("[%s] Beginning overlap failed", fnName);
    }

    /* End overlap */
    uint32_t a2[] = {1, 2, 8, 9, 10};
    uint32_t b2[] = {5, 6, 8, 9, 10};
    uint32_t exp2[] = {8, 9, 10};
    if (!testIntersection(fn, fnName, a2, 5, b2, 5, exp2, 3)) {
        ERR("[%s] End overlap failed", fnName);
    }

    /* Middle overlap */
    uint32_t a3[] = {1, 2, 5, 6, 7, 20, 21};
    uint32_t b3[] = {3, 4, 5, 6, 7, 30, 31};
    uint32_t exp3[] = {5, 6, 7};
    if (!testIntersection(fn, fnName, a3, 7, b3, 7, exp3, 3)) {
        ERR("[%s] Middle overlap failed", fnName);
    }

    /* Sparse overlap */
    uint32_t a4[] = {1, 5, 10, 15, 20, 25, 30};
    uint32_t b4[] = {2, 5, 12, 15, 22, 25, 32};
    uint32_t exp4[] = {5, 15, 25};
    if (!testIntersection(fn, fnName, a4, 7, b4, 7, exp4, 3)) {
        ERR("[%s] Sparse overlap failed", fnName);
    }

    return err;
}

/* Test small arrays (below SIMD width) */
static int32_t testSmallArrays(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    /* 2-element arrays */
    uint32_t a1[] = {10, 20};
    uint32_t b1[] = {15, 20};
    uint32_t exp1[] = {20};
    if (!testIntersection(fn, fnName, a1, 2, b1, 2, exp1, 1)) {
        ERR("[%s] 2-element arrays failed", fnName);
    }

    /* 3-element arrays */
    uint32_t a2[] = {5, 10, 15};
    uint32_t b2[] = {5, 12, 15};
    uint32_t exp2[] = {5, 15};
    if (!testIntersection(fn, fnName, a2, 3, b2, 3, exp2, 2)) {
        ERR("[%s] 3-element arrays failed", fnName);
    }

    return err;
}

/* Test larger arrays that use SIMD paths */
static int32_t testLargeArrays(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    /* Arrays longer than 128 elements to trigger SIMD paths */
    uint32_t a[200];
    uint32_t b[200];
    uint32_t expected[100];
    size_t expLen = 0;

    /* a = even numbers 0-398, b = multiples of 3 from 0-597 */
    for (size_t i = 0; i < 200; i++) {
        a[i] = (uint32_t)(i * 2);
        b[i] = (uint32_t)(i * 3);
    }

    /* Expected: numbers divisible by both 2 and 3 (i.e., 6) in range */
    for (uint32_t v = 0; v < 398 && expLen < 100; v += 6) {
        if (v < 597) {
            expected[expLen++] = v;
        }
    }

    if (!testIntersection(fn, fnName, a, 200, b, 200, expected, expLen)) {
        ERR("[%s] Large arrays test failed", fnName);
    }

    return err;
}

/* Test different size ratios (triggers different algorithm paths) */
static int32_t testSizeRatios(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    /* Small vs large (10x ratio - uses v1) */
    uint32_t small1[] = {50, 100, 150, 200, 250};
    uint32_t large1[64];
    for (size_t i = 0; i < 64; i++) {
        large1[i] = (uint32_t)(i * 4); /* 0, 4, 8, ..., 252 */
    }
    uint32_t exp1[] = {100, 200};
    if (!testIntersection(fn, fnName, small1, 5, large1, 64, exp1, 2)) {
        ERR("[%s] 10x ratio test failed", fnName);
    }

    return err;
}

/* Test commutativity: intersect(A,B) == intersect(B,A) */
static int32_t testCommutativity(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    uint32_t a[] = {1, 3, 5, 7, 9, 11, 13, 15};
    uint32_t b[] = {2, 3, 5, 8, 11, 14, 15, 20};
    uint32_t out1[16], out2[16];

    size_t len1 = fn(a, 8, b, 8, out1);
    size_t len2 = fn(b, 8, a, 8, out2);

    if (len1 != len2) {
        ERR("[%s] Commutativity length mismatch: %" PRIu64 " vs %" PRIu64,
            fnName, (uint64_t)len1, (uint64_t)len2);
    }

    for (size_t i = 0; i < len1; i++) {
        if (out1[i] != out2[i]) {
            ERR("[%s] Commutativity value mismatch at %" PRIu64, fnName,
                (uint64_t)i);
        }
    }

    return err;
}

/* Test with larger values (near uint32_t max) */
static int32_t testLargeValues(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    uint32_t a[] = {UINT32_MAX - 10, UINT32_MAX - 5, UINT32_MAX - 2,
                    UINT32_MAX - 1, UINT32_MAX};
    uint32_t b[] = {UINT32_MAX - 8, UINT32_MAX - 5, UINT32_MAX - 3,
                    UINT32_MAX - 1};
    uint32_t expected[] = {UINT32_MAX - 5, UINT32_MAX - 1};
    if (!testIntersection(fn, fnName, a, 5, b, 4, expected, 2)) {
        ERR("[%s] Large values test failed", fnName);
    }

    return err;
}

/* Run all tests for a specific intersection function */
static int32_t runTestsForFunction(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    err |= testEmptyArrays(fn, fnName);
    err |= testSingleElements(fn, fnName);
    err |= testDisjoint(fn, fnName);
    err |= testIdentical(fn, fnName);
    err |= testPartialOverlap(fn, fnName);
    err |= testSmallArrays(fn, fnName);
    err |= testLargeArrays(fn, fnName);
    err |= testSizeRatios(fn, fnName);
    err |= testCommutativity(fn, fnName);
    err |= testLargeValues(fn, fnName);

    return err;
}

/* Wrapper functions for asymmetric APIs */
static size_t wrapV1(const uint32_t *a, size_t lenA, const uint32_t *b,
                     size_t lenB, uint32_t *out) {
    if (lenA == 0 || lenB == 0) {
        return 0;
    }
    if (lenA <= lenB) {
        return v1(a, lenA, b, lenB, out);
    }
    return v1(b, lenB, a, lenA, out);
}

static size_t wrapV3(const uint32_t *a, size_t lenA, const uint32_t *b,
                     size_t lenB, uint32_t *out) {
    if (lenA == 0 || lenB == 0) {
        return 0;
    }
    if (lenA <= lenB) {
        return v3(a, lenA, b, lenB, out);
    }
    return v3(b, lenB, a, lenA, out);
}

static size_t wrapSIMDgalloping(const uint32_t *a, size_t lenA, const uint32_t *b,
                                size_t lenB, uint32_t *out) {
    if (lenA == 0 || lenB == 0) {
        return 0;
    }
    if (lenA <= lenB) {
        return SIMDgalloping(a, lenA, b, lenB, out);
    }
    return SIMDgalloping(b, lenB, a, lenA, out);
}

#if __AVX2__
static size_t wrapV1AVX2(const uint32_t *a, size_t lenA, const uint32_t *b,
                         size_t lenB, uint32_t *out) {
    if (lenA == 0 || lenB == 0) {
        return 0;
    }
    if (lenA <= lenB) {
        return v1_avx2(a, lenA, b, lenB, out);
    }
    return v1_avx2(b, lenB, a, lenA, out);
}

static size_t wrapV3AVX2(const uint32_t *a, size_t lenA, const uint32_t *b,
                         size_t lenB, uint32_t *out) {
    if (lenA == 0 || lenB == 0) {
        return 0;
    }
    if (lenA <= lenB) {
        return v3_avx2(a, lenA, b, lenB, out);
    }
    return v3_avx2(b, lenB, a, lenA, out);
}

static size_t wrapSIMDgallopingAVX2(const uint32_t *a, size_t lenA,
                                    const uint32_t *b, size_t lenB,
                                    uint32_t *out) {
    if (lenA == 0 || lenB == 0) {
        return 0;
    }
    if (lenA <= lenB) {
        return SIMDgalloping_avx2(a, lenA, b, lenB, out);
    }
    return SIMDgalloping_avx2(b, lenB, a, lenA, out);
}
#endif

/* Test that all algorithms produce identical results */
static int32_t testConsistency(void) {
    int32_t err = 0;

    /* Generate test arrays */
    uint32_t a[256], b[256];
    for (size_t i = 0; i < 256; i++) {
        a[i] = (uint32_t)(i * 2 + 1);  /* odd numbers 1-511 */
        b[i] = (uint32_t)(i * 3);      /* multiples of 3: 0-765 */
    }

    uint32_t out_scalar[256];
    uint32_t out_intersect[256];
    uint32_t out_gallop[256];

    size_t len_scalar = scalar(a, 256, b, 256, out_scalar);
    size_t len_intersect = intersectInt(a, 256, b, 256, out_intersect);
    size_t len_gallop =
        intersectIntOneSidedGalloping(a, 256, b, 256, out_gallop);

    if (len_scalar != len_intersect) {
        ERR("Consistency: scalar (%" PRIu64 ") vs intersectInt (%" PRIu64 ")",
            (uint64_t)len_scalar, (uint64_t)len_intersect);
    }

    if (len_scalar != len_gallop) {
        ERR("Consistency: scalar (%" PRIu64 ") vs galloping (%" PRIu64 ")",
            (uint64_t)len_scalar, (uint64_t)len_gallop);
    }

    for (size_t i = 0; i < len_scalar; i++) {
        if (out_scalar[i] != out_intersect[i]) {
            ERR("Consistency mismatch scalar vs intersect at %" PRIu64,
                (uint64_t)i);
        }
        if (out_scalar[i] != out_gallop[i]) {
            ERR("Consistency mismatch scalar vs gallop at %" PRIu64,
                (uint64_t)i);
        }
    }

#if __AVX2__
    uint32_t out_avx2[256];
    size_t len_avx2 = intersectIntAVX2(a, 256, b, 256, out_avx2);

    if (len_scalar != len_avx2) {
        ERR("Consistency: scalar (%" PRIu64 ") vs AVX2 (%" PRIu64 ")",
            (uint64_t)len_scalar, (uint64_t)len_avx2);
    }

    for (size_t i = 0; i < len_scalar; i++) {
        if (out_scalar[i] != out_avx2[i]) {
            ERR("Consistency mismatch scalar vs AVX2 at %" PRIu64, (uint64_t)i);
        }
    }
#endif

    return err;
}

int intersectIntTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    printf("Testing scalar...\n");
    err |= runTestsForFunction(scalar, "scalar");

    printf("Testing match_scalar...\n");
    err |= runTestsForFunction(match_scalar, "match_scalar");

    printf("Testing intersectInt (main heuristic)...\n");
    err |= runTestsForFunction(intersectInt, "intersectInt");

    printf("Testing intersectIntOneSidedGalloping...\n");
    err |= runTestsForFunction(intersectIntOneSidedGalloping,
                               "intersectIntOneSidedGalloping");

    printf("Testing v1 (wrapped)...\n");
    err |= runTestsForFunction(wrapV1, "v1");

    printf("Testing v3 (wrapped)...\n");
    err |= runTestsForFunction(wrapV3, "v3");

    printf("Testing SIMDgalloping (wrapped)...\n");
    err |= runTestsForFunction(wrapSIMDgalloping, "SIMDgalloping");

    printf("Testing highlyscalable_intersect_SIMD...\n");
    err |= runTestsForFunction(highlyscalable_intersect_SIMD,
                               "highlyscalable_intersect_SIMD");

    printf("Testing lemire_highlyscalable_intersect_SIMD...\n");
    err |= runTestsForFunction(lemire_highlyscalable_intersect_SIMD,
                               "lemire_highlyscalable_intersect_SIMD");

#if __AVX2__
    printf("Testing intersectIntAVX2...\n");
    err |= runTestsForFunction(intersectIntAVX2, "intersectIntAVX2");

    printf("Testing v1_avx2 (wrapped)...\n");
    err |= runTestsForFunction(wrapV1AVX2, "v1_avx2");

    printf("Testing v3_avx2 (wrapped)...\n");
    err |= runTestsForFunction(wrapV3AVX2, "v3_avx2");

    printf("Testing SIMDgalloping_avx2 (wrapped)...\n");
    err |= runTestsForFunction(wrapSIMDgallopingAVX2, "SIMDgalloping_avx2");
#endif

    printf("Testing cross-algorithm consistency...\n");
    err |= testConsistency();

    TEST_FINAL_RESULT;
}
#endif
