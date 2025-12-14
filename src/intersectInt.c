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

/* x86 SIMD intrinsics - only available on x86/x64 */
#if defined(__x86_64__) || defined(__i386__)
#include <pmmintrin.h>
#include <smmintrin.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#if USE_ALIGNED
#define MM_LOAD_SI_128 _mm_load_si128
#define MM_STORE_SI_128 _mm_store_si128
#else
#define MM_LOAD_SI_128 _mm_loadu_si128
// #define MM_STORE_SI_128 _mm_storeu_si128
#endif
#endif /* x86 */

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
    if ((lower >= length) or(array[lower] >= min)) {
        return lower;
    }

    size_t spansize = 1; // could set larger
    // bootstrap an upper limit

    while ((lower + spansize < length) and(array[lower + spansize] < min)) {
        spansize *= 2;
    }

    size_t upper = (lower + spansize < length) ? lower + spansize : length - 1;

    if (array[upper] < min) { // means array has no item >= min
        return length;
    }

    // we know that the next-smallest span was too small
    lower += (spansize / 2);

    // else begin binary search
    while (lower + 1 != upper) {
        size_t mid = (lower + upper) / 2;
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

/* ============================================================================
 * x86 SIMD implementations (SSE/AVX2)
 * ============================================================================
 */
#if defined(__x86_64__) || defined(__i386__)

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
    if ((length1 == 0) or(length2 == 0)) {
        return 0;
    }

    if ((1000 * length1 <= length2) or(1000 * length2 <= length1)) {
        if (length1 <= length2) {
            return SIMDgalloping(set1, length1, set2, length2, out);
        }

        return SIMDgalloping(set2, length2, set1, length1, out);
    }

    if ((50 * length1 <= length2) or(50 * length2 <= length1)) {
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

#elif defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
/* ============================================================================
 * ARM NEON SIMD implementations
 * ============================================================================
 */
#include <arm_neon.h>

/**
 * ARM NEON version of v1 intersection algorithm.
 * Intersects two sorted arrays using NEON SIMD operations.
 */
size_t v1_neon(const uint32_t *rare, size_t lenRare, const uint32_t *freq,
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

    if ((rare >= stopRare) || (freq >= stopFreq)) {
        goto FINISH_SCALAR;
    }

    uint32_t valRare = rare[0];
    uint64_t maxFreq = freq[2 * 4 - 1];

    uint32x4_t Rare = vdupq_n_u32(valRare);
    uint32x4_t F0 = vld1q_u32(freq);
    uint32x4_t F1 = vld1q_u32(freq + 4);

    if (maxFreq < valRare) {
        goto ADVANCE_FREQ;
    }

ADVANCE_RARE:
    do {
        *matchOut = valRare;
        rare += 1;
        if (rare >= stopRare) {
            rare -= 1;
            goto FINISH_SCALAR;
        }

        valRare = rare[0];
        uint32x4_t cmp0 = vceqq_u32(F0, Rare);
        uint32x4_t cmp1 = vceqq_u32(F1, Rare);
        Rare = vdupq_n_u32(valRare);
        uint32x4_t combined = vorrq_u32(cmp0, cmp1);

        /* Check if any match found using horizontal max */
        if (vmaxvq_u32(combined) != 0) {
            matchOut++;
        }

        F0 = vld1q_u32(freq);
        F1 = vld1q_u32(freq + 4);

    } while (maxFreq >= valRare);

    uint64_t maxProbe;

ADVANCE_FREQ:
    do {
        const uint64_t kProbe = (0 + 1) * 2 * 4;
        const uint32_t *probeFreq = freq + kProbe;

        if (probeFreq >= stopFreq) {
            goto FINISH_SCALAR;
        }

        maxProbe = freq[(0 + 2) * 2 * 4 - 1];
        freq = probeFreq;

    } while (maxProbe < valRare);

    maxFreq = maxProbe;

    F0 = vld1q_u32(freq);
    F1 = vld1q_u32(freq + 4);

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
 * ARM NEON version of v3 intersection algorithm.
 * Better for larger size differences between arrays.
 */
size_t v3_neon(const uint32_t *rare, const size_t lenRare, const uint32_t *freq,
               const size_t lenFreq, uint32_t *out) {
    if (lenFreq == 0 || lenRare == 0) {
        return 0;
    }

    assert(lenRare <= lenFreq);
    const uint32_t *const initout = out;
    const uint32_t *stopFreq = freq + lenFreq - 8;
    const uint32_t *stopRare = rare + lenRare;

    if (freq > stopFreq) {
        return match_scalar(freq, lenFreq, rare, lenRare, out);
    }

    while (rare < stopRare && freq <= stopFreq) {
        const uint32_t matchRare = *rare;
        uint32x4_t Match = vdupq_n_u32(matchRare);

        /* Skip freq chunks until we might find a match */
        while (freq[7] < matchRare) {
            freq += 8;
            if (freq > stopFreq) {
                goto FINISH_SCALAR;
            }
        }

        uint32x4_t Q0 = vld1q_u32(freq);
        uint32x4_t Q1 = vld1q_u32(freq + 4);

        uint32x4_t cmp0 = vceqq_u32(Q0, Match);
        uint32x4_t cmp1 = vceqq_u32(Q1, Match);
        uint32x4_t combined = vorrq_u32(cmp0, cmp1);

        if (vmaxvq_u32(combined) != 0) {
            *out++ = matchRare;
        }
        rare++;
    }

FINISH_SCALAR:;
    /* Handle remaining elements with scalar code */
    size_t freqRemain = (stopFreq + 8) - freq;
    size_t rareRemain = stopRare - rare;
    size_t tailCount = match_scalar(freq, freqRemain, rare, rareRemain, out);

    return (out - initout) + tailCount;
}

/**
 * ARM NEON version of SIMD galloping intersection.
 * Good for highly skewed distributions.
 */
size_t SIMDgalloping_neon(const uint32_t *rare, const size_t lenRare,
                          const uint32_t *freq, const size_t lenFreq,
                          uint32_t *out) {
    if (lenFreq == 0 || lenRare == 0) {
        return 0;
    }

    assert(lenRare <= lenFreq);
    const uint32_t *const initout = out;
    const uint32_t *stopFreq = freq + lenFreq - 8;
    const uint32_t *stopRare = rare + lenRare;

    if (freq > stopFreq) {
        return intersectIntOneSidedGalloping(rare, lenRare, freq, lenFreq, out);
    }

    while (rare < stopRare) {
        const uint32_t matchRare = *rare;
        uint32x4_t Match = vdupq_n_u32(matchRare);

        /* Galloping search */
        size_t jump = 8;
        while (freq + jump <= stopFreq && freq[jump + 7] < matchRare) {
            freq += jump;
            jump *= 2;
        }

        /* Binary search refinement within jump range */
        while (freq <= stopFreq && freq[7] < matchRare) {
            freq += 8;
        }

        if (freq > stopFreq) {
            break;
        }

        uint32x4_t Q0 = vld1q_u32(freq);
        uint32x4_t Q1 = vld1q_u32(freq + 4);

        uint32x4_t cmp0 = vceqq_u32(Q0, Match);
        uint32x4_t cmp1 = vceqq_u32(Q1, Match);
        uint32x4_t combined = vorrq_u32(cmp0, cmp1);

        if (vmaxvq_u32(combined) != 0) {
            *out++ = matchRare;
        }
        rare++;
    }

    /* Handle remaining with scalar galloping */
    size_t freqRemain = (stopFreq + 8) - freq;
    size_t rareRemain = stopRare - rare;
    size_t tailCount =
        intersectIntOneSidedGalloping(rare, rareRemain, freq, freqRemain, out);

    return (out - initout) + tailCount;
}

/**
 * ARM NEON main heuristic - selects best algorithm based on size ratio.
 */
size_t intersectInt(const uint32_t *set1, const size_t length1,
                    const uint32_t *set2, const size_t length2, uint32_t *out) {
    if ((length1 == 0) || (length2 == 0)) {
        return 0;
    }

    if ((1000 * length1 <= length2) || (1000 * length2 <= length1)) {
        if (length1 <= length2) {
            return SIMDgalloping_neon(set1, length1, set2, length2, out);
        }
        return SIMDgalloping_neon(set2, length2, set1, length1, out);
    }

    if ((50 * length1 <= length2) || (50 * length2 <= length1)) {
        if (length1 <= length2) {
            return v3_neon(set1, length1, set2, length2, out);
        }
        return v3_neon(set2, length2, set1, length1, out);
    }

    if (length1 <= length2) {
        return v1_neon(set1, length1, set2, length2, out);
    }

    return v1_neon(set2, length2, set1, length1, out);
}

#else /* non-x86, non-ARM: use scalar fallback */

/**
 * Scalar fallback for platforms without SIMD support.
 */
size_t intersectInt(const uint32_t *set1, const size_t length1,
                    const uint32_t *set2, const size_t length2, uint32_t *out) {
    if ((length1 == 0) || (length2 == 0)) {
        return 0;
    }

    return scalar(set1, length1, set2, length2, out);
}

#endif /* platform selection */

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
    if ((length1 == 0) or(length2 == 0)) {
        return 0;
    }

    if ((1000 * length1 <= length2) or(1000 * length2 <= length1)) {
        if (length1 <= length2) {
            return SIMDgalloping_avx2(set1, length1, set2, length2, out);
        }

        return SIMDgalloping_avx2(set2, length2, set1, length1, out);
    }

    if ((50 * length1 <= length2) or(50 * length2 <= length1)) {
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

/* Additional x86 SIMD functions */
#if defined(__x86_64__) || defined(__i386__)

/**
 * Shuffle mask table for compacting matching elements to the front of SSE
 * register. More or less from
 * http://highlyscalable.wordpress.com/2012/06/05/fast-intersection-sorted-lists-sse/
 *
 * For a 4-bit mask where bit i indicates element i matched, this table provides
 * the _mm_shuffle_epi8 pattern to move matching elements to positions 0,1,2,...
 *
 * Element layout in 128-bit register: elem0=bytes[0-3], elem1=bytes[4-7],
 * elem2=bytes[8-11], elem3=bytes[12-15]. Unused positions filled with 0x80
 * (zeros output).
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

#endif /* x86 for highlyscalable functions */

#ifdef DATAKIT_TEST
#include "ctest.h"

#include <inttypes.h> /* PRIu64, PRIu32 */
#include <string.h>   /* memset */

/* ====================================================================
 * Helper functions for tests
 * ==================================================================== */
typedef size_t (*intersectFn)(const uint32_t *, size_t, const uint32_t *,
                              size_t, uint32_t *);

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
    const uint32_t a1[] = {42};
    const uint32_t b1[] = {42};
    const uint32_t exp1[] = {42};
    if (!testIntersection(fn, fnName, a1, 1, b1, 1, exp1, 1)) {
        ERR("[%s] Single match failed", fnName);
    }

    /* Single element, no match */
    const uint32_t a2[] = {10};
    const uint32_t b2[] = {20};
    if (!testIntersection(fn, fnName, a2, 1, b2, 1, NULL, 0)) {
        ERR("[%s] Single no-match failed", fnName);
    }

    /* Single vs multiple, match */
    const uint32_t a3[] = {5};
    const uint32_t b3[] = {1, 3, 5, 7, 9};
    const uint32_t exp3[] = {5};
    if (!testIntersection(fn, fnName, a3, 1, b3, 5, exp3, 1)) {
        ERR("[%s] Single vs multiple match failed", fnName);
    }

    /* Single vs multiple, no match */
    const uint32_t a4[] = {6};
    const uint32_t b4[] = {1, 3, 5, 7, 9};
    if (!testIntersection(fn, fnName, a4, 1, b4, 5, NULL, 0)) {
        ERR("[%s] Single vs multiple no-match failed", fnName);
    }

    return err;
}

/* Test disjoint sets (no common elements) */
static int32_t testDisjoint(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    /* Interleaved disjoint */
    const uint32_t a[] = {2, 4, 6, 8, 10};
    const uint32_t b[] = {1, 3, 5, 7, 9};
    if (!testIntersection(fn, fnName, a, 5, b, 5, NULL, 0)) {
        ERR("[%s] Interleaved disjoint failed", fnName);
    }

    /* Non-overlapping ranges */
    const uint32_t a2[] = {1, 2, 3, 4, 5};
    const uint32_t b2[] = {10, 20, 30, 40, 50};
    if (!testIntersection(fn, fnName, a2, 5, b2, 5, NULL, 0)) {
        ERR("[%s] Non-overlapping ranges failed", fnName);
    }

    return err;
}

/* Test identical sets */
static int32_t testIdentical(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    const uint32_t a[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint32_t b[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint32_t expected[] = {1, 2, 3, 4, 5, 6, 7, 8};
    if (!testIntersection(fn, fnName, a, 8, b, 8, expected, 8)) {
        ERR("[%s] Identical sets failed", fnName);
    }

    return err;
}

/* Test partial overlap */
static int32_t testPartialOverlap(intersectFn fn, const char *fnName) {
    int32_t err = 0;

    /* Beginning overlap */
    const uint32_t a1[] = {1, 2, 3, 4, 5};
    const uint32_t b1[] = {1, 2, 3, 10, 20};
    const uint32_t exp1[] = {1, 2, 3};
    if (!testIntersection(fn, fnName, a1, 5, b1, 5, exp1, 3)) {
        ERR("[%s] Beginning overlap failed", fnName);
    }

    /* End overlap */
    const uint32_t a2[] = {1, 2, 8, 9, 10};
    const uint32_t b2[] = {5, 6, 8, 9, 10};
    const uint32_t exp2[] = {8, 9, 10};
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

    // cppcheck-suppress uninitvar - loop above always executes (v=0 < 398),
    // expected is initialized
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

/* Wrapper functions for asymmetric APIs - x86 only */
#if defined(__x86_64__) || defined(__i386__)
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

static size_t wrapSIMDgalloping(const uint32_t *a, size_t lenA,
                                const uint32_t *b, size_t lenB, uint32_t *out) {
    if (lenA == 0 || lenB == 0) {
        return 0;
    }
    if (lenA <= lenB) {
        return SIMDgalloping(a, lenA, b, lenB, out);
    }
    return SIMDgalloping(b, lenB, a, lenA, out);
}
#endif /* x86 wrappers */

/* Wrapper functions for ARM NEON APIs */
#if defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
static size_t wrapV1Neon(const uint32_t *a, size_t lenA, const uint32_t *b,
                         size_t lenB, uint32_t *out) {
    if (lenA == 0 || lenB == 0) {
        return 0;
    }
    if (lenA <= lenB) {
        return v1_neon(a, lenA, b, lenB, out);
    }
    return v1_neon(b, lenB, a, lenA, out);
}

static size_t wrapV3Neon(const uint32_t *a, size_t lenA, const uint32_t *b,
                         size_t lenB, uint32_t *out) {
    if (lenA == 0 || lenB == 0) {
        return 0;
    }
    if (lenA <= lenB) {
        return v3_neon(a, lenA, b, lenB, out);
    }
    return v3_neon(b, lenB, a, lenA, out);
}

static size_t wrapSIMDgallopingNeon(const uint32_t *a, size_t lenA,
                                    const uint32_t *b, size_t lenB,
                                    uint32_t *out) {
    if (lenA == 0 || lenB == 0) {
        return 0;
    }
    if (lenA <= lenB) {
        return SIMDgalloping_neon(a, lenA, b, lenB, out);
    }
    return SIMDgalloping_neon(b, lenB, a, lenA, out);
}
#endif /* ARM NEON wrappers */

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

/* ====================================================================
 * Comprehensive Stress Tests for Scalar vs SIMD Consistency
 * ==================================================================== */

/* Simple linear congruential generator for reproducible random numbers */
static uint32_t stressTestRand(uint32_t *seed) {
    *seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
    return *seed;
}

/* Compare two intersection results - returns number of errors */
static int32_t compareResults(const char *testName, const uint32_t *out1,
                              size_t len1, const uint32_t *out2, size_t len2,
                              const char *name1, const char *name2) {
    int32_t err = 0;
    if (len1 != len2) {
        printf("  [%s] FAIL: %s returned %" PRIu64 ", %s returned %" PRIu64
               "\n",
               testName, name1, (uint64_t)len1, name2, (uint64_t)len2);
        return 1;
    }
    for (size_t i = 0; i < len1; i++) {
        if (out1[i] != out2[i]) {
            printf("  [%s] FAIL: mismatch at index %" PRIu64 ": %s=%" PRIu32
                   ", %s=%" PRIu32 "\n",
                   testName, (uint64_t)i, name1, out1[i], name2, out2[i]);
            err++;
            if (err >= 10) {
                printf("  [%s] ... (stopping after 10 errors)\n", testName);
                break;
            }
        }
    }
    return err > 0 ? 1 : 0;
}

/* Stress test: Various array sizes */
static int32_t stressTestSizes(void) {
    int32_t err = 0;
    printf("  Testing various array sizes...\n");

    /* Test sizes that exercise different code paths */
    size_t sizes[] = {0,   1,   2,   3,   4,    7,    8,    9,    15,  16,  17,
                      31,  32,  33,  63,  64,   65,   100,  127,  128, 129, 255,
                      256, 257, 500, 512, 1000, 1024, 2000, 4096, 8192};
    size_t numSizes = sizeof(sizes) / sizeof(sizes[0]);

    /* Allocate buffers for largest test */
    uint32_t *a = malloc(8192 * sizeof(uint32_t));
    uint32_t *b = malloc(8192 * sizeof(uint32_t));
    uint32_t *out_scalar = malloc(8192 * sizeof(uint32_t));
    uint32_t *out_simd = malloc(8192 * sizeof(uint32_t));

    if (!a || !b || !out_scalar || !out_simd) {
        printf("  FAIL: Memory allocation failed\n");
        free(a);
        free(b);
        free(out_scalar);
        free(out_simd);
        return 1;
    }

    for (size_t si = 0; si < numSizes; si++) {
        size_t sizeA = sizes[si];
        for (size_t sj = 0; sj < numSizes; sj++) {
            size_t sizeB = sizes[sj];

            /* Generate sequential arrays with 50% expected overlap */
            for (size_t i = 0; i < sizeA; i++) {
                a[i] = (uint32_t)(i * 2);
            }
            for (size_t i = 0; i < sizeB; i++) {
                b[i] = (uint32_t)(i * 2 + (i % 2)); /* Interleaved */
            }

            size_t len_scalar = scalar(a, sizeA, b, sizeB, out_scalar);
            size_t len_simd = intersectInt(a, sizeA, b, sizeB, out_simd);

            char testName[64];
            snprintf(testName, sizeof(testName), "size_%zu_x_%zu", sizeA,
                     sizeB);

            err |= compareResults(testName, out_scalar, len_scalar, out_simd,
                                  len_simd, "scalar", "intersectInt");
        }
    }

    free(a);
    free(b);
    free(out_scalar);
    free(out_simd);
    return err;
}

/* Stress test: Random data with various densities */
static int32_t stressTestRandom(void) {
    int32_t err = 0;
    printf("  Testing random data patterns...\n");

    const size_t maxSize = 10000;
    uint32_t *a = malloc(maxSize * sizeof(uint32_t));
    uint32_t *b = malloc(maxSize * sizeof(uint32_t));
    uint32_t *out_scalar = malloc(maxSize * sizeof(uint32_t));
    uint32_t *out_simd = malloc(maxSize * sizeof(uint32_t));

    if (!a || !b || !out_scalar || !out_simd) {
        printf("  FAIL: Memory allocation failed\n");
        free(a);
        free(b);
        free(out_scalar);
        free(out_simd);
        return 1;
    }

    /* Test with different random seeds and densities */
    uint32_t seeds[] = {12345, 67890, 11111, 99999, 0xDEADBEEF};
    size_t testSizes[] = {100, 500, 1000, 5000, 10000};

    for (size_t seedIdx = 0; seedIdx < 5; seedIdx++) {
        for (size_t sizeIdx = 0; sizeIdx < 5; sizeIdx++) {
            uint32_t seed = seeds[seedIdx];
            size_t size = testSizes[sizeIdx];

            /* Generate sorted random arrays */
            uint32_t val = 0;
            for (size_t i = 0; i < size; i++) {
                val += 1 + (stressTestRand(&seed) % 10);
                a[i] = val;
            }

            val = stressTestRand(&seed) % 5; /* Different starting point */
            for (size_t i = 0; i < size; i++) {
                val += 1 + (stressTestRand(&seed) % 10);
                b[i] = val;
            }

            size_t len_scalar = scalar(a, size, b, size, out_scalar);
            size_t len_simd = intersectInt(a, size, b, size, out_simd);

            char testName[64];
            snprintf(testName, sizeof(testName), "random_seed%u_size%zu",
                     seeds[seedIdx], size);

            err |= compareResults(testName, out_scalar, len_scalar, out_simd,
                                  len_simd, "scalar", "intersectInt");
        }
    }

    free(a);
    free(b);
    free(out_scalar);
    free(out_simd);
    return err;
}

/* Stress test: Edge cases */
static int32_t stressTestEdgeCases(void) {
    int32_t err = 0;
    printf("  Testing edge cases...\n");

    const size_t maxSize = 1000;
    uint32_t *a = malloc(maxSize * sizeof(uint32_t));
    uint32_t *b = malloc(maxSize * sizeof(uint32_t));
    uint32_t *out_scalar = malloc(maxSize * sizeof(uint32_t));
    uint32_t *out_simd = malloc(maxSize * sizeof(uint32_t));

    if (!a || !b || !out_scalar || !out_simd) {
        printf("  FAIL: Memory allocation failed\n");
        free(a);
        free(b);
        free(out_scalar);
        free(out_simd);
        return 1;
    }

    /* Test 1: No overlap - disjoint ranges */
    for (size_t i = 0; i < 500; i++) {
        a[i] = (uint32_t)i;          /* 0-499 */
        b[i] = (uint32_t)(i + 1000); /* 1000-1499 */
    }
    {
        size_t len_scalar = scalar(a, 500, b, 500, out_scalar);
        size_t len_simd = intersectInt(a, 500, b, 500, out_simd);
        err |= compareResults("no_overlap", out_scalar, len_scalar, out_simd,
                              len_simd, "scalar", "intersectInt");
        if (len_scalar != 0 || len_simd != 0) {
            printf("  [no_overlap] FAIL: Expected 0 results\n");
            err = 1;
        }
    }

    /* Test 2: Complete overlap - identical arrays */
    for (size_t i = 0; i < 500; i++) {
        a[i] = (uint32_t)(i * 2);
        b[i] = (uint32_t)(i * 2);
    }
    {
        size_t len_scalar = scalar(a, 500, b, 500, out_scalar);
        size_t len_simd = intersectInt(a, 500, b, 500, out_simd);
        err |= compareResults("complete_overlap", out_scalar, len_scalar,
                              out_simd, len_simd, "scalar", "intersectInt");
        if (len_scalar != 500) {
            printf(
                "  [complete_overlap] FAIL: Expected 500 results, got %" PRIu64
                "\n",
                (uint64_t)len_scalar);
            err = 1;
        }
    }

    /* Test 3: Single element overlap at start */
    a[0] = 42;
    for (size_t i = 1; i < 500; i++) {
        a[i] = (uint32_t)(i + 100);
    }
    b[0] = 42;
    for (size_t i = 1; i < 500; i++) {
        b[i] = (uint32_t)(i + 1000);
    }
    {
        size_t len_scalar = scalar(a, 500, b, 500, out_scalar);
        size_t len_simd = intersectInt(a, 500, b, 500, out_simd);
        err |= compareResults("single_overlap_start", out_scalar, len_scalar,
                              out_simd, len_simd, "scalar", "intersectInt");
        if (len_scalar != 1 || out_scalar[0] != 42) {
            printf("  [single_overlap_start] FAIL: Expected [42]\n");
            err = 1;
        }
    }

    /* Test 4: Single element overlap at end */
    for (size_t i = 0; i < 499; i++) {
        a[i] = (uint32_t)i;
    }
    a[499] = 99999;
    for (size_t i = 0; i < 499; i++) {
        b[i] = (uint32_t)(i + 1000);
    }
    b[499] = 99999;
    {
        size_t len_scalar = scalar(a, 500, b, 500, out_scalar);
        size_t len_simd = intersectInt(a, 500, b, 500, out_simd);
        err |= compareResults("single_overlap_end", out_scalar, len_scalar,
                              out_simd, len_simd, "scalar", "intersectInt");
        if (len_scalar != 1 || out_scalar[0] != 99999) {
            printf("  [single_overlap_end] FAIL: Expected [99999]\n");
            err = 1;
        }
    }

    /* Test 5: All same values */
    for (size_t i = 0; i < 500; i++) {
        a[i] = 12345;
        b[i] = 12345;
    }
    {
        size_t len_scalar = scalar(a, 500, b, 500, out_scalar);
        size_t len_simd = intersectInt(a, 500, b, 500, out_simd);
        err |= compareResults("all_same", out_scalar, len_scalar, out_simd,
                              len_simd, "scalar", "intersectInt");
    }

    /* Test 6: Alternating overlap (every other element) */
    for (size_t i = 0; i < 500; i++) {
        a[i] = (uint32_t)(i * 2); /* 0, 2, 4, 6, ... */
        b[i] = (uint32_t)(i);     /* 0, 1, 2, 3, ... */
    }
    {
        size_t len_scalar = scalar(a, 500, b, 500, out_scalar);
        size_t len_simd = intersectInt(a, 500, b, 500, out_simd);
        err |= compareResults("alternating", out_scalar, len_scalar, out_simd,
                              len_simd, "scalar", "intersectInt");
    }

    /* Test 7: Large values near UINT32_MAX */
    for (size_t i = 0; i < 500; i++) {
        a[i] = UINT32_MAX - 1000 + (uint32_t)(i * 2);
        b[i] = UINT32_MAX - 1000 + (uint32_t)(i * 2);
    }
    {
        size_t len_scalar = scalar(a, 500, b, 500, out_scalar);
        size_t len_simd = intersectInt(a, 500, b, 500, out_simd);
        err |= compareResults("large_values", out_scalar, len_scalar, out_simd,
                              len_simd, "scalar", "intersectInt");
    }

    /* Test 8: One tiny array, one large array (tests galloping) */
    a[0] = 5000;
    a[1] = 15000;
    a[2] = 25000;
    for (size_t i = 0; i < 1000; i++) {
        b[i] = (uint32_t)(i * 30); /* 0, 30, 60, ... 29970 */
    }
    {
        size_t len_scalar = scalar(a, 3, b, 1000, out_scalar);
        size_t len_simd = intersectInt(a, 3, b, 1000, out_simd);
        err |= compareResults("tiny_vs_large", out_scalar, len_scalar, out_simd,
                              len_simd, "scalar", "intersectInt");
    }

    free(a);
    free(b);
    free(out_scalar);
    free(out_simd);
    return err;
}

/* Stress test: Skewed size ratios (exercises galloping paths) */
static int32_t stressTestSkewedRatios(void) {
    int32_t err = 0;
    printf("  Testing skewed size ratios (galloping paths)...\n");

    const size_t maxSize = 50000;
    uint32_t *a = malloc(maxSize * sizeof(uint32_t));
    uint32_t *b = malloc(maxSize * sizeof(uint32_t));
    uint32_t *out_scalar = malloc(maxSize * sizeof(uint32_t));
    uint32_t *out_simd = malloc(maxSize * sizeof(uint32_t));

    if (!a || !b || !out_scalar || !out_simd) {
        printf("  FAIL: Memory allocation failed\n");
        free(a);
        free(b);
        free(out_scalar);
        free(out_simd);
        return 1;
    }

    /* Test ratios that trigger different algorithm selections:
     * - ratio >= 1000:1 triggers SIMDgalloping
     * - ratio >= 50:1 triggers v3
     * - otherwise uses v1 */
    size_t smallSizes[] = {1, 5, 10, 20, 50, 100};
    size_t largeSizes[] = {1000, 5000, 10000, 50000};

    for (size_t si = 0; si < 6; si++) {
        for (size_t li = 0; li < 4; li++) {
            size_t smallSize = smallSizes[si];
            size_t largeSize = largeSizes[li];

            /* Generate arrays with known overlap */
            for (size_t i = 0; i < smallSize; i++) {
                a[i] = (uint32_t)(i * 1000); /* Sparse: 0, 1000, 2000, ... */
            }
            for (size_t i = 0; i < largeSize; i++) {
                b[i] = (uint32_t)i; /* Dense: 0, 1, 2, ... */
            }

            size_t len_scalar = scalar(a, smallSize, b, largeSize, out_scalar);
            size_t len_simd =
                intersectInt(a, smallSize, b, largeSize, out_simd);

            char testName[64];
            snprintf(testName, sizeof(testName), "ratio_%zu_to_%zu", smallSize,
                     largeSize);

            err |= compareResults(testName, out_scalar, len_scalar, out_simd,
                                  len_simd, "scalar", "intersectInt");

            /* Also test with reversed order */
            len_scalar = scalar(b, largeSize, a, smallSize, out_scalar);
            len_simd = intersectInt(b, largeSize, a, smallSize, out_simd);

            snprintf(testName, sizeof(testName), "ratio_%zu_to_%zu_rev",
                     largeSize, smallSize);

            err |= compareResults(testName, out_scalar, len_scalar, out_simd,
                                  len_simd, "scalar", "intersectInt");
        }
    }

    free(a);
    free(b);
    free(out_scalar);
    free(out_simd);
    return err;
}

/* Stress test: Platform-specific SIMD functions directly */
static int32_t stressTestPlatformSIMD(void) {
    int32_t err = 0;
    printf("  Testing platform-specific SIMD implementations...\n");

    const size_t maxSize = 10000;
    uint32_t *a = malloc(maxSize * sizeof(uint32_t));
    uint32_t *b = malloc(maxSize * sizeof(uint32_t));
    uint32_t *out_scalar = malloc(maxSize * sizeof(uint32_t));
    uint32_t *out_v1 = malloc(maxSize * sizeof(uint32_t));
    uint32_t *out_v3 = malloc(maxSize * sizeof(uint32_t));
    uint32_t *out_gallop = malloc(maxSize * sizeof(uint32_t));

    if (!a || !b || !out_scalar || !out_v1 || !out_v3 || !out_gallop) {
        printf("  FAIL: Memory allocation failed\n");
        free(a);
        free(b);
        free(out_scalar);
        free(out_v1);
        free(out_v3);
        free(out_gallop);
        return 1;
    }

    uint32_t seed = 0xCAFEBABE;
    size_t sizes[] = {100, 500, 1000, 2000, 5000};

    for (size_t sizeIdx = 0; sizeIdx < 5; sizeIdx++) {
        size_t size = sizes[sizeIdx];

        /* Generate random sorted arrays */
        uint32_t val = 0;
        for (size_t i = 0; i < size; i++) {
            val += 1 + (stressTestRand(&seed) % 5);
            a[i] = val;
        }
        val = stressTestRand(&seed) % 3;
        for (size_t i = 0; i < size; i++) {
            val += 1 + (stressTestRand(&seed) % 5);
            b[i] = val;
        }

        size_t len_scalar = scalar(a, size, b, size, out_scalar);

#if defined(__x86_64__) || defined(__i386__)
        /* Test x86 SIMD functions */
        size_t len_v1 = v1(a, size, b, size, out_v1);
        size_t len_v3 = v3(a, size, b, size, out_v3);
        size_t len_gallop = SIMDgalloping(a, size, b, size, out_gallop);

        char testName[64];
        snprintf(testName, sizeof(testName), "x86_v1_size%zu", size);
        err |= compareResults(testName, out_scalar, len_scalar, out_v1, len_v1,
                              "scalar", "v1");

        snprintf(testName, sizeof(testName), "x86_v3_size%zu", size);
        err |= compareResults(testName, out_scalar, len_scalar, out_v3, len_v3,
                              "scalar", "v3");

        snprintf(testName, sizeof(testName), "x86_gallop_size%zu", size);
        err |= compareResults(testName, out_scalar, len_scalar, out_gallop,
                              len_gallop, "scalar", "SIMDgalloping");
#endif

#if defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
        /* Test ARM NEON functions */
        size_t len_v1 = v1_neon(a, size, b, size, out_v1);
        size_t len_v3 = v3_neon(a, size, b, size, out_v3);
        size_t len_gallop = SIMDgalloping_neon(a, size, b, size, out_gallop);

        char testName[64];
        snprintf(testName, sizeof(testName), "neon_v1_size%zu", size);
        err |= compareResults(testName, out_scalar, len_scalar, out_v1, len_v1,
                              "scalar", "v1_neon");

        snprintf(testName, sizeof(testName), "neon_v3_size%zu", size);
        err |= compareResults(testName, out_scalar, len_scalar, out_v3, len_v3,
                              "scalar", "v3_neon");

        snprintf(testName, sizeof(testName), "neon_gallop_size%zu", size);
        err |= compareResults(testName, out_scalar, len_scalar, out_gallop,
                              len_gallop, "scalar", "SIMDgalloping_neon");
#endif
    }

    free(a);
    free(b);
    free(out_scalar);
    free(out_v1);
    free(out_v3);
    free(out_gallop);
    return err;
}

/* Stress test: Boundary conditions around SIMD vector widths */
static int32_t stressTestBoundaries(void) {
    int32_t err = 0;
    printf("  Testing SIMD boundary conditions...\n");

    /* Test sizes around 4 (SSE/NEON 128-bit = 4x32-bit) and 8 (AVX 256-bit) */
    size_t sizes[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                      13, 14, 15, 16, 17, 31, 32, 33, 63, 64, 65};
    size_t numSizes = sizeof(sizes) / sizeof(sizes[0]);

    uint32_t a[128], b[128], out_scalar[128], out_simd[128];

    for (size_t si = 0; si < numSizes; si++) {
        size_t size = sizes[si];

        /* Generate consecutive arrays with 100% overlap */
        for (size_t i = 0; i < size; i++) {
            a[i] = (uint32_t)(i + 1);
            b[i] = (uint32_t)(i + 1);
        }

        // cppcheck-suppress uninitvar - sizes array contains only non-zero
        // values, a and b always initialized
        size_t len_scalar = scalar(a, size, b, size, out_scalar);
        size_t len_simd = intersectInt(a, size, b, size, out_simd);

        char testName[64];
        snprintf(testName, sizeof(testName), "boundary_full_overlap_%zu", size);
        err |= compareResults(testName, out_scalar, len_scalar, out_simd,
                              len_simd, "scalar", "intersectInt");

        /* Generate arrays with 0% overlap */
        for (size_t i = 0; i < size; i++) {
            a[i] = (uint32_t)(i);
            b[i] = (uint32_t)(i + 1000);
        }

        len_scalar = scalar(a, size, b, size, out_scalar);
        len_simd = intersectInt(a, size, b, size, out_simd);

        snprintf(testName, sizeof(testName), "boundary_no_overlap_%zu", size);
        err |= compareResults(testName, out_scalar, len_scalar, out_simd,
                              len_simd, "scalar", "intersectInt");
    }

    return err;
}

/* Main stress test runner */
static int32_t stressTestScalarVsSIMD(void) {
    int32_t err = 0;

    err |= stressTestSizes();
    err |= stressTestRandom();
    err |= stressTestEdgeCases();
    err |= stressTestSkewedRatios();
    err |= stressTestPlatformSIMD();
    err |= stressTestBoundaries();

    return err;
}

/* Test that all algorithms produce identical results */
static int32_t testConsistency(void) {
    int32_t err = 0;

    /* Generate test arrays */
    uint32_t a[256], b[256];
    for (size_t i = 0; i < 256; i++) {
        a[i] = (uint32_t)(i * 2 + 1); /* odd numbers 1-511 */
        b[i] = (uint32_t)(i * 3);     /* multiples of 3: 0-765 */
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

#if defined(__x86_64__) || defined(__i386__)
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
#endif /* x86 tests */

#if defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
    printf("Testing v1_neon (wrapped)...\n");
    err |= runTestsForFunction(wrapV1Neon, "v1_neon");

    printf("Testing v3_neon (wrapped)...\n");
    err |= runTestsForFunction(wrapV3Neon, "v3_neon");

    printf("Testing SIMDgalloping_neon (wrapped)...\n");
    err |= runTestsForFunction(wrapSIMDgallopingNeon, "SIMDgalloping_neon");
#endif /* ARM NEON tests */

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

    printf("Running scalar vs SIMD stress tests...\n");
    err |= stressTestScalarVsSIMD();

    printf("Testing cross-algorithm consistency...\n");
    err |= testConsistency();

    TEST_FINAL_RESULT;
}
#endif
