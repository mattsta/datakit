#include "float16.h"

/* Public domain code from:
 * https://stackoverflow.com/questions/1659440
 *
 * It's basically unreadable unless you go read the float16 and float32
 * IEEE specs, so we just trust it as-is. All our testing checks out so far. */

/* This does match the output of the float32 to float16 intrinsic _cvtss_sh(),
 * so we can switch between machine float16 and function float16 encode/decode
 * as necessary:
 *     assert(float16Encode(val) == _cvtss_sh(val, 0));
 */

/* An alternative table-based implementation is presented in:
 * ftp://ftp.fox-toolkit.org/pub/fasthalffloatconversion.pdf
 * but it requires about 1500 bytes of tables for full functionality */

/* This encoding has:
 * 49152  fractional representations (yay! more than just trunc regular floats)
 * 14336  integer representations (boo; WE WANT FLOATS)
 *     1  inf
 *     1  -inf
 *  2046  NaN
 * =========================================
 * 65536  total representations, as expected */

typedef union Bits {
    float f;
    int32_t si;
    uint32_t ui;
} Bits;

#define shift 13
#define shiftSign 16

#define infN 0x7F800000  // flt32 infinity
#define maxN 0x477FE000  // max flt16 normal as a flt32
#define minN 0x38800000  // min flt16 normal as a flt32
#define signN 0x80000000 // flt32 sign bit

#define infC (infN >> shift)
#define nanN ((infC + 1) << shift) // minimum flt16 nan as a flt32
#define maxC (maxN >> shift)
#define minC (minN >> shift)
#define signC (signN >> shiftSign) // flt16 sign bit

#define mulN 0x52000000 // (1 << 23) / minN
#define mulC 0x33800000 // minN / (1 << (23 - shift))

#define subC 0x003FF // max flt32 subnormal down shifted
#define norC 0x00400 // min flt32 normal down shifted

#define maxD (infC - maxC - 1)
#define minD (minC - subC - 1)

/* Ignore sanitizer because it doesn't like multiplying floats into ints
 * ("outside the range of representation") */
__attribute__((no_sanitize("float-cast-overflow"))) uint16_t
float16Encode_(const float value) {
    Bits v = {.f = value};
    Bits s = {.si = mulN};
    uint32_t sign = v.si & signN;

    v.si ^= sign;
    sign >>= shiftSign; // logical shift
    // cppcheck-suppress overlappingWriteUnion
    s.si = s.f * v.f; // correct subnormals (intentional union type-punning)
    v.si ^= (s.si ^ v.si) & -(minN > v.si);
    v.si ^= (infN ^ v.si) & -((infN > v.si) & (v.si > maxN));
    v.si ^= (nanN ^ v.si) & -((nanN > v.si) & (v.si > infN));
    v.ui >>= shift; // logical shift
    v.si ^= ((v.si - maxD) ^ v.si) & -(v.si > maxC);
    v.si ^= ((v.si - minD) ^ v.si) & -(v.si > subC);

    return v.ui | sign;
}

__attribute__((no_sanitize("shift"))) float
float16Decode_(const uint16_t value) {
    Bits v = {.ui = value};
    int32_t sign = v.si & signC;
    Bits s = {.si = mulC};

    v.si ^= sign;
    sign <<= shiftSign;
    v.si ^= ((v.si + minD) ^ v.si) & -(v.si > subC);
    v.si ^= ((v.si + maxD) ^ v.si) & -(v.si > maxC);

    s.f *= v.si;
    const int32_t mask = -(norC > v.si);
    v.si <<= shift;
    v.si ^= (s.si ^ v.si) & mask;
    v.si |= sign;

    return v.f;
}

#include "endianIsLittle.h"
uint16_t bfloat16Encode_(const float value) {
    if (endianIsLittle()) {
        const uint16_t *MSB = (uint16_t *)&value;
        return MSB[1];
    }

    return *(uint16_t *)&value;
}

float bfloat16Decode_(const uint16_t value) {
    if (endianIsLittle()) {
        union v {
            uint16_t ret[2];
            float value;
        } got = {.ret[1] = value};
        return got.value;
    }

    return (float)value;
}

#ifdef DATAKIT_TEST
#include "perf.h"
#include <assert.h>

#define REPORT_TIME 1
#if REPORT_TIME
#define TIME_INIT PERF_TIMERS_SETUP
#define TIME_FINISH(i, what) PERF_TIMERS_FINISH_PRINT_RESULTS(i, what)
#else
#define TIME_INIT
#define TIME_FINISH(i, what)
#endif

#if __F16C__
#include <immintrin.h>
#endif

__attribute__((unused)) static void yieldAllFloats(void) {
    printf("float16 floats\n");
    for (size_t i = 0; i <= UINT16_MAX; i++) {
        printf("%zu: %.16g\n", i, float16Decode(i));
    }

    printf("\n");
    printf("bfloat16 floats\n");
    for (size_t i = 0; i <= UINT16_MAX; i++) {
        printf("%zu: %.16g\n", i, bfloat16Decode(i));
    }
}

__attribute__((no_sanitize("integer", "undefined")))
__attribute__((optnone)) int
float16Test(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const size_t testers = 1ULL << 30;
    const float in = 3.333333333333333333333;
    const uint16_t created = 44444;

    size_t incr = 0;
    float incrf = 0.0;

#if 0
    yieldAllFloats();
#endif

#if 0
    const float tester = -3.658203125;
    printf("Encoded -3.6582 as: %.16f\n", bfloat16Decode(bfloat16Encode(tester)));
    printf("Encoded -3.6582 as: %.16f\n", float16Decode(float16Encode(tester)));
    for (size_t i = 0; i < UINT16_MAX; i++) {
        const float converted16 = float16Decode(i);
        const float convertedB16 = bfloat16Decode(bfloat16Encode(converted16));
        if (convertedB16 != converted16) {
            printf("float16 %.16f encoded to bfloat16 as %.16f\n", converted16, convertedB16);
        }
    }

    for (size_t i = 0; i < UINT16_MAX; i++) {
        const float converted16 = bfloat16Decode(i);
        const float convertedB16 = float16Decode(float16Encode(converted16));
        if (convertedB16 != converted16) {
            printf("bfloat16 %.16g encoded to float16 as %.16g\n", converted16, convertedB16);
        }
    }
#endif

#if __F16C__
    const float floats[8] = {in, in, in, in, in, in, in, in};
    __m128i half_vector;
#endif

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            assert(true);
        }

        TIME_FINISH(testers, "assert overhead");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incr += float16Encode_(in);
        }

        TIME_FINISH(testers, "float16Encode software");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incrf += float16Decode_(created);
        }

        TIME_FINISH(testers, "float16Decode software");
    }

#if __F16C__
    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incr += _cvtss_sh(in, 0);
        }

        TIME_FINISH(testers, "float16Encode hardware");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incrf += _cvtsh_ss(created);
        }

        TIME_FINISH(testers, "float16Decode hardware");
    }
#endif

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incr += bfloat16Encode(in);
        }

        TIME_FINISH(testers, "bfloat16Encode software");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incrf += bfloat16Decode(in);
        }

        TIME_FINISH(testers, "bfloat16Decode software");
    }

#if __F16C__
    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            __m256 float_vector = _mm256_load_ps(floats);
            half_vector = _mm256_cvtps_ph(float_vector, 0);
            incr += half_vector[0];
        }

        TIME_FINISH(testers * 8, "float16Encode Vector Hardware");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            // uint16_t halfs[16] = {0};
            __m256 float_vector = _mm256_cvtph_ps(half_vector);
            //_mm256_store_si256 ((__m256i*)halfs, // half_vector);
            incrf += float_vector[0];
        }

        TIME_FINISH(testers * 8, "float16Decode Vector Hardware");
    }
#endif

    /*************************************************/
    printf("==========================================\n\n");
    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            Bits v = {.f = in};
            Bits s = {.si = mulN};
            uint32_t sign = v.si & signN;

            v.si ^= sign;
            sign >>= shiftSign; // logical shift
            s.si = s.f * v.f;   // correct subnormals
            v.si ^= (s.si ^ v.si) & -(minN > v.si);
            v.si ^= (infN ^ v.si) & -((infN > v.si) & (v.si > maxN));
            v.si ^= (nanN ^ v.si) & -((nanN > v.si) & (v.si > infN));
            v.ui >>= shift; // logical shift
            v.si ^= ((v.si - maxD) ^ v.si) & -(v.si > maxC);
            v.si ^= ((v.si - minD) ^ v.si) & -(v.si > subC);

            const uint16_t got = v.ui | sign;
            incr += got;
        }

        TIME_FINISH(testers, "float16Encode software (inline; optnone)");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            Bits v = {.ui = in};
            int32_t sign = v.si & signC;
            Bits s = {.si = mulC};

            v.si ^= sign;
            sign <<= shiftSign;
            v.si ^= ((v.si + minD) ^ v.si) & -(v.si > subC);
            v.si ^= ((v.si + maxD) ^ v.si) & -(v.si > maxC);

            s.f *= v.si;
            const int32_t mask = -(norC > v.si);
            v.si <<= shift;
            v.si ^= (s.si ^ v.si) & mask;
            v.si |= sign;
            const float got = v.f;
            incrf += got;
        }

        TIME_FINISH(testers, "float16Decode software (inline; optnone)");
    }

#if __F16C__
    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incr += _cvtss_sh(in, 0);
        }

        TIME_FINISH(testers, "float16Encode hardware (inline)");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incrf += _cvtsh_ss(created);
        }

        TIME_FINISH(testers, "float16Decode hardware (inline)");
    }
#endif

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            const uint16_t *MSB = (uint16_t *)&in;
            const uint16_t got = MSB[1];
            incr += got;
        }

        TIME_FINISH(testers, "bfloat16Encode software (inline)");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            union v {
                uint16_t ret[2];
                float value;
            } got = {.ret[1] = created};
            const float gv = got.value;
            incrf += gv;
        }

        TIME_FINISH(testers, "bfloat16Decode software (inline)");
    }

    return incr + incrf;
}

#endif
