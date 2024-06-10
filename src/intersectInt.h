#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Given two arrays, this writes the intersection to out. Returns the
 * cardinality of the intersection.
 *
 * This is a mix of very fast vectorized intersection algorithms, several
 * designed by N. Kurz, with adaptations by D. Lemire.
 */
size_t intersectInt(const uint32_t *set1, const size_t length1,
                    const uint32_t *set2, const size_t length2, uint32_t *out);

#if __AVX2__
#include <immintrin.h>
/*
 * Direct port of intersectInt to AVX2.
 */
size_t intersectIntAVX2(const uint32_t *set1, const size_t length1,
                        const uint32_t *set2, const size_t length2,
                        uint32_t *out);
#endif

#if __AVX2__
#define intersectIntAuto(a, b, c, d, e) intersectIntAVX2(a, b, c, d, e)
#else
#define intersectIntAuto(a, b, c, d, e) intersectInt(a, b, c, d, e)
#endif

/*
 * Given two arrays, this writes the intersection to out. Returns the
 * cardinality of the intersection.
 *
 * This applies a state-of-the-art algorithm. First coded by O. Kaser, adapted
 * by D. Lemire.
 */
size_t intersectIntOneSidedGalloping(const uint32_t *smallset,
                                     const size_t smalllength,
                                     const uint32_t *largeset,
                                     const size_t largelength, uint32_t *out);
