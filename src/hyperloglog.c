/* hyperloglog — HLL container refactored, modernized, and optimized.
 *
 * Improvements and modifications:
 * Copyright 2016-2019 Matt Stancliff <matt@genges.com>
 *
 * See end of file for original provenance.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hyperloglog.h"

#include "datakit.h"
#include "mdsc.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h> /* va_arg */

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#if 0
#define XXH_FORCE_MEMORY_ACCESS 2
#endif
#define XXH_NOSTATE 1
#include "../deps/xxHash/xxhash.h"

#if 0
#define HYPERHASH XXH64
#else
#define HYPERHASH XXH3_64bits_withSeed
#endif

/* SIMD support for optimized HLL operations */
#if __AVX__ || __SSE2__
#define HLL_USE_SSE 1
#include <immintrin.h>
#endif

#if defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
#define HLL_USE_NEON 1
#include <arm_neon.h>
#endif

/* 'hyperloglog' is actually an mdsc, but we cast the
 * start of data in the mdsc to 'hyperloglogHeader'
 * for accessing internal fields.
 *
 * Header encodes:
 *  - cached cardinality (61 bits)
 *  - cached cardinality validity (is number here valid?) (1 bit)
 *  - encoding (3 values requiring 2 bits: sparse, dense, raw)
 *  - variable length data.
 *
 * The mdsc itself caches the total length of the HLL external to this struct,
 * so we don't need to maintain length of registers[] in the header. */
/* TODO:
 *   - when clang adds support, add:
 *     - __attribute__ ((scalar_storage_order("little-endian"))) */
typedef struct hyperloglogHeader {
    uint64_t cardinality : 61;     /* Cached cardinality */
    uint64_t cardinalityValid : 1; /* Boolean, is cardinality valid? */
    uint64_t encoding : 2;         /* Encoding; one of SPARSE, DENSE, or RAW */
    uint8_t registers[];           /* Data bytes. */
} __attribute__((packed)) hyperloglogHeader;

_Static_assert(sizeof(hyperloglogHeader) == sizeof(uint64_t),
               "Header got too big?");

/* Big ugly compile-time endian check macro so we can properly
 * complain to the user if we are on an untested platform. */
#if (defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN) ||              \
    defined(__LITTLE_ENDIAN__) || defined(__ARMEL__) ||                        \
    defined(__THUMBEL__) || defined(__AARCH64EL__) || defined(_MIPSEL) ||      \
    defined(__MIPSEL) || defined(__MIPSEL__)
/* OK */
#else
#warning "Didn't detect a little endian system. "
"Persisting and reading persisted HLLs may not work reliably."
#endif

/* This needs to be configurable later, but it's a holdover from when
 * integration was baked directly into the server config. */
static const uint64_t serverSparseMaxBytes = 4096;

/* This HyperLogLog implementation is based on the following ideas:
 *
 * * Use a 64 bit hash function as proposed in [1], for
 *   cardinalities up to 10^9, at the cost of just 1 additional
 *   bit per register.
 * * Use 16384 6-bit registers for higher accuracy, using
 *   a total of 12 KiB per key.
 * * The use of the mdsc string data type. No new type is introduced.
 * * No attempt is made to compress the data structure as in [1]. Also the
 *   algorithm used is the original HyperLogLog Algorithm as in [2], with
 *   the only difference that a 64 bit hash function is used, so no correction
 *   is performed for values near 2^32 as in [1].
 *
 * [1] Heule, Nunkesser, Hall: HyperLogLog in Practice: Algorithmic
 *     Engineering of a State of The Art Cardinality Estimation Algorithm.
 *
 * [2] P. Flajolet, Éric Fusy, O. Gandouet, and F. Meunier. Hyperloglog: The
 *     analysis of a near-optimal cardinality estimation algorithm.
 *
 * This HLL has two representations:
 *
 * 1) A "dense" representation where every entry is represented by
 *    a 6-bit integer.
 * 2) A "sparse" representation using run length compression suitable
 *    for representing HyperLogLogs with many registers set to 0 in
 *    a memory efficient way.
 *
 *
 * HLL header
 * ===
 *
 * Both the dense and sparse representation have 8 byte header as
 * defined by hyperloglogHeader.
 *
 * When 'cardinalityValid' false, the data structure was modified and
 * we can't reuse the cached value that must be recomputed.
 *
 * Dense representation
 * ===
 *
 * The dense representation used here is the following:
 *
 * +--------+--------+--------+------//      //--+
 * |11000000|22221111|33333322|55444444 ....     |
 * +--------+--------+--------+------//      //--+
 *
 * The 6 bits counters are encoded one after the other starting from the
 * LSB to the MSB, and using the next bytes as needed.
 *
 * Sparse representation
 * ===
 *
 * The sparse representation encodes registers using a run length
 * encoding composed of three opcodes, two using one byte, and one using
 * of two bytes. The opcodes are called ZERO, XZERO and VAL.
 *
 * ZERO opcode is represented as 00xxxxxx. The 6-bit integer represented
 * by the six bits 'xxxxxx', plus 1, means that there are N registers set
 * to 0. This opcode can represent from 1 to 64 contiguous registers set
 * to the value of 0.
 *
 * XZERO opcode is represented by two bytes 01xxxxxx yyyyyyyy. The 14-bit
 * integer represented by the bits 'xxxxxx' as most significant bits and
 * 'yyyyyyyy' as least significant bits, plus 1, means that there are N
 * registers set to 0. This opcode can represent from 0 to 16384 contiguous
 * registers set to the value of 0.
 *
 * VAL opcode is represented as 1vvvvvxx. It contains a 5-bit integer
 * representing the value of a register, and a 2-bit integer representing
 * the number of contiguous registers set to that value 'vvvvv'.
 * To obtain the value and run length, the integers vvvvv and xx must be
 * incremented by one. This opcode can represent values from 1 to 32,
 * repeated from 1 to 4 times.
 *
 * The sparse representation can't represent registers with a value greater
 * than 32, however it is very unlikely that we find such a register in an
 * HLL with a cardinality where the sparse representation is still more
 * memory efficient than the dense representation. When this happens the
 * HLL is converted to the dense representation.
 *
 * The sparse representation is purely positional. For example a sparse
 * representation of an empty HLL is just: XZERO:16384.
 *
 * An HLL having only 3 non-zero registers at position 1000, 1020, 1021
 * respectively set to 2, 3, 3, is represented by the following three
 * opcodes:
 *
 * XZERO:1000 (Registers 0-999 are set to 0)
 * VAL:2,1    (1 register set to value 2, that is register 1000)
 * ZERO:19    (Registers 1001-1019 set to 0)
 * VAL:3,2    (2 registers set to value 3, that is registers 1020,1021)
 * XZERO:15362 (Registers 1022-16383 set to 0)
 *
 * In the example the sparse representation used just 7 bytes instead
 * of 12k in order to represent the HLL registers. In general for low
 * cardinality there is a big win in terms of space efficiency, traded
 * with CPU time since the sparse representation is slower to access:
 *
 * The following table shows average cardinality vs bytes used, 100
 * samples per cardinality (when the set was not representable because
 * of registers with too big value, the dense representation size was used
 * as a sample).
 *
 * 100 267
 * 200 485
 * 300 678
 * 400 859
 * 500 1033
 * 600 1205
 * 700 1375
 * 800 1544
 * 900 1713
 * 1000 1882
 * 2000 3480
 * 3000 4879
 * 4000 6089
 * 5000 7138
 * 6000 8042
 * 7000 8823
 * 8000 9500
 * 9000 10088
 * 10000 10591
 *
 * The dense representation uses 12288 bytes, so there is a big win up to
 * a cardinality of ~2000-3000. For bigger cardinalities the constant times
 * involved in updating the sparse representation is not justified by the
 * memory savings. The exact maximum length of the sparse representation
 * when this implementation switches to the dense representation is
 * configured via the define server.hyperloglogSparseMaxBytes.
 */

#define toHLL(mdsc_) ((hyperloglog *)(mdsc_))
#define toHLLIn(mdsc_) ((hyperloglog **)&(mdsc_))
#define toMDSC(hyperloglog_) ((mdsc *)(hyperloglog_))

#define HLL_MERGE(max, current, failure)                                       \
    do {                                                                       \
        /* Merge with this HLL with our 'max' HHL by setting max[i]            \
         * to MAX(max[i],hyperloglog[i]). */                                   \
        if (hyperloglogMerge((max), (current)) == false) {                     \
            return (failure);                                                  \
        }                                                                      \
    } while (0)

/* The cached cardinality signals validity of the cached value. */
#define HLL_INVALIDATE_CACHE(hdr)                                              \
    do {                                                                       \
        (hdr)->cardinalityValid = false;                                       \
    } while (0)

#define HLL_VALID_CACHE(hdr) ((hdr)->cardinalityValid)

#define HLL_CARDINALITY_MAX (1ULL << 61)

#define HLL_P 14           /* The greater is P, the smaller the error. */
#define HLL_Q (64 - HLL_P) /* bits of hash for determining leading zeroes */
#define HLL_REGISTERS (1 << HLL_P)     /* With P=14, 16384 registers. */
#define HLL_P_MASK (HLL_REGISTERS - 1) /* Mask to index register. */
#define HLL_BITS 6 /* Enough to count up to 63 leading zeroes. */
#define HLL_REGISTER_MAX ((1 << HLL_BITS) - 1)
#define HLL_HDR_SIZE sizeof(hyperloglogHeader)
#define HLL_DENSE_SIZE (HLL_HDR_SIZE + ((HLL_REGISTERS * HLL_BITS + 7) / 8))
#define HLL_MAX_SIZE HLL_HDR_SIZE + HLL_REGISTERS
#define HLL_MAX_ENCODING 1
#define HLL_ZERO_BOUND ((sizeof(uint64_t) * 8) - HLL_P)

typedef uint8_t hllStatic[HLL_MAX_SIZE];

typedef enum hyperloglogEncoding {
    /* Keep DENSE as 0 so 0-initialized hllStatic is dense with no setting. */
    HLL_DENSE = 0,
    HLL_SPARSE = 1,
    HLL_RAW = 2, /* Only used internally, never exposed to user storage. */
    HLL_MAX_UNUSED = 3
} hyperloglogEncoding;

#define isDense(hll) ((hll)->encoding == HLL_DENSE)
#define isSparse(hll) ((hll)->encoding == HLL_SPARSE)

/* =========================== Low level bit macros ========================= */

/* Macros to access the dense representation.
 *
 * We need to get and set 6 bit counters in an array of 8 bit bytes.
 * We use macros to make sure the code is inlined since speed is critical
 * especially in order to compute the approximated cardinality in
 * HLLCOUNT where we need to access all the registers at once.
 * For the same reason we also want to avoid conditionals in this code path.
 *
 * +--------+--------+--------+------//
 * |11000000|22221111|33333322|55444444
 * +--------+--------+--------+------//
 *
 * Note: in the above representation the most significant bit (MSB)
 * of every byte is on the left. We start using bits from the LSB to MSB,
 * and so forth passing to the next byte.
 *
 * Example, we want to access to counter at pos = 1 ("111111" in the
 * illustration above).
 *
 * The index of the first byte b0 containing our data is:
 *
 *  b0 = 6 * pos / 8 = 0
 *
 *   +--------+
 *   |11000000|  <- Our byte at b0
 *   +--------+
 *
 * The position of the first bit (counting from the LSB = 0) in the byte
 * is given by:
 *
 *  fb = 6 * pos % 8 -> 6
 *
 * Right shift b0 of 'fb' bits.
 *
 *   +--------+
 *   |11000000|  <- Initial value of b0
 *   |00000011|  <- After right shift of 6 pos.
 *   +--------+
 *
 * Left shift b1 of bits 8-fb bits (2 bits)
 *
 *   +--------+
 *   |22221111|  <- Initial value of b1
 *   |22111100|  <- After left shift of 2 bits.
 *   +--------+
 *
 * OR the two bits, and finally AND with 111111 (63 in decimal) to
 * clean the higher order bits we are not interested in:
 *
 *   +--------+
 *   |00000011|  <- b0 right shifted
 *   |22111100|  <- b1 left shifted
 *   |22111111|  <- b0 OR b1
 *   |  111111|  <- (b0 OR b1) AND 63, our value.
 *   +--------+
 *
 * We can try with a different example, like pos = 0. In this case
 * the 6-bit counter is actually contained in a single byte.
 *
 *  b0 = 6 * pos / 8 = 0
 *
 *   +--------+
 *   |11000000|  <- Our byte at b0
 *   +--------+
 *
 *  fb = 6 * pos % 8 = 0
 *
 *  So we right shift of 0 bits (no shift in practice) and
 *  left shift the next byte of 8 bits, even if we don't use it,
 *  but this has the effect of clearing the bits so the result
 *  will not be affacted after the OR.
 *
 * -------------------------------------------------------------------------
 *
 * Setting the register is a bit more complex, let's assume that 'val'
 * is the value we want to set, already in the right range.
 *
 * We need two steps, in one we need to clear the bits, and in the other
 * we need to bitwise-OR the new bits.
 *
 * Let's try with 'pos' = 1, so our first byte at 'b' is 0,
 *
 * "fb" is 6 in this case.
 *
 *   +--------+
 *   |11000000|  <- Our byte at b0
 *   +--------+
 *
 * To create a AND-mask to clear the bits about this position, we just
 * initialize the mask with the value 63, left shift it of "fs" bits,
 * and finally invert the result.
 *
 *   +--------+
 *   |00111111|  <- "mask" starts at 63
 *   |11000000|  <- "mask" after left shift of "ls" bits.
 *   |00111111|  <- "mask" after invert.
 *   +--------+
 *
 * Now we can bitwise-AND the byte at "b" with the mask, and bitwise-OR
 * it with "val" left-shifted of "ls" bits to set the new bits.
 *
 * Now let's focus on the next byte b1:
 *
 *   +--------+
 *   |22221111|  <- Initial value of b1
 *   +--------+
 *
 * To build the AND mask we start again with the 63 value, right shift
 * it by 8-fb bits, and invert it.
 *
 *   +--------+
 *   |00111111|  <- "mask" set at 2&6-1
 *   |00001111|  <- "mask" after the right shift by 8-fb = 2 bits
 *   |11110000|  <- "mask" after bitwise not.
 *   +--------+
 *
 * Now we can mask it with b+1 to clear the old bits, and bitwise-OR
 * with "val" left-shifted by "rs" bits to set the new value.
 */

/* Note: if we access the last counter, we will also access the b+1 byte
 * that is out of the array, but mdsc strings always have an implicit null
 * term, so the byte exists, and we can skip the conditional (or the need
 * to allocate 1 byte more explicitly). */

/* Store the value of the register at position 'regnum' into variable 'target'.
 * 'p' is an array of unsigned bytes. */
#define HLL_DENSE_GET_REGISTER(target, p, regnum)                              \
    do {                                                                       \
        uint8_t *_p = (uint8_t *)p;                                            \
        uint64_t _byte = regnum * HLL_BITS / 8;                                \
        uint64_t _fb = regnum * HLL_BITS & 7;                                  \
        uint64_t _fb8 = 8 - _fb;                                               \
        uint64_t b0 = _p[_byte];                                               \
        uint64_t b1 = _p[_byte + 1];                                           \
        target = ((b0 >> _fb) | (b1 << _fb8)) & HLL_REGISTER_MAX;              \
    } while (0)

/* Set the value of the register at position 'regnum' to 'val'.
 * 'p' is an array of unsigned bytes. */
#define HLL_DENSE_SET_REGISTER(p, regnum, val)                                 \
    do {                                                                       \
        uint8_t *_p = (uint8_t *)p;                                            \
        uint64_t _byte = regnum * HLL_BITS / 8;                                \
        uint64_t _fb = regnum * HLL_BITS & 7;                                  \
        uint64_t _fb8 = 8 - _fb;                                               \
        uint64_t _v = val;                                                     \
        _p[_byte] &= ~(HLL_REGISTER_MAX << _fb);                               \
        _p[_byte] |= _v << _fb;                                                \
        _p[_byte + 1] &= ~(HLL_REGISTER_MAX >> _fb8);                          \
        _p[_byte + 1] |= _v >> _fb8;                                           \
    } while (0)

/* Macros to access the sparse representation.
 * The macros parameter is expected to be an uint8_t pointer. */
#define HLL_SPARSE_XZERO_BIT 0x40                    /* 01xxxxxx */
#define HLL_SPARSE_VAL_BIT 0x80                      /* 1vvvvvxx */
#define HLL_SPARSE_IS_ZERO(p) (((*(p)) & 0xc0) == 0) /* 00xxxxxx */
#define HLL_SPARSE_IS_XZERO(p) (((*(p)) & 0xc0) == HLL_SPARSE_XZERO_BIT)
#define HLL_SPARSE_IS_VAL(p) ((*(p)) & HLL_SPARSE_VAL_BIT)
#define HLL_SPARSE_ZERO_LEN(p) (((*(p)) & 0x3f) + 1)
#define HLL_SPARSE_XZERO_LEN(p) (((((*(p)) & 0x3f) << 8) | (*((p) + 1))) + 1)
#define HLL_SPARSE_VAL_VALUE(p) ((((*(p)) >> 2) & 0x1f) + 1)
#define HLL_SPARSE_VAL_LEN(p) (((*(p)) & 0x3) + 1)
#define HLL_SPARSE_VAL_MAX_VALUE 32
#define HLL_SPARSE_VAL_MAX_LEN 4
#define HLL_SPARSE_ZERO_MAX_LEN 64
#define HLL_SPARSE_XZERO_MAX_LEN 16384
#define HLL_SPARSE_VAL_SET(p, val, len)                                        \
    do {                                                                       \
        *(p) = (((val) - 1) << 2 | ((len) - 1)) | HLL_SPARSE_VAL_BIT;          \
    } while (0)
#define HLL_SPARSE_ZERO_SET(p, len)                                            \
    do {                                                                       \
        *(p) = (len) - 1;                                                      \
    } while (0)
#define HLL_SPARSE_XZERO_SET(p, len)                                           \
    do {                                                                       \
        int32_t _l = (len) - 1;                                                \
        *(p) = (_l >> 8) | HLL_SPARSE_XZERO_BIT;                               \
        *((p) + 1) = (_l & 0xff);                                              \
    } while (0)

#define HLL_ALPHA_INF 0.721347520444481703680 /* constant for 0.5/ln(2) */

/* ========================= HyperLogLog algorithm  ========================= */

/* Given a string element to add to the HyperLogLog, returns the length
 * of the pattern 000..1 of the element hash. As a side effect 'regp' is
 * set to the register index this element hashes to. */
DK_STATIC int_fast32_t hyperloglogPatLen(const void *data_, size_t len,
                                         int64_t *regp) {
    uint64_t hash;

    /* Count the number of zeroes starting from bit HLL_REGISTERS
     * (that is a power of two corresponding to the first bit we don't use
     * as index). The max run can be 64-P+1 bits = Q+1 bits.
     *
     * Note the final "1" ending the sequence of zeroes must be
     * included in the count, so if we find "001" the count is 3, and
     * the smallest count possible is no zeroes at all, just a 1 bit
     * at the first position, that is a count of 1.
     *
     * This may sound like inefficient, but actually in the average case
     * there are high probabilities to find a 1 after a few iterations. */
    hash = HYPERHASH(data_, len, 0xadc83b19ULL);
    *regp = hash & HLL_P_MASK; /* Register index. */
    hash >>= HLL_P;            /* Remove bits used to address the register. */
    hash |= (1ULL << HLL_Q);   /* Verify count will be <= Q + 1 */

    /* ctzll can't handle 0 as an input, so we need to special-case
     * the unlikely scenario */
    if (unlikely(hash == 0)) {
        return HLL_ZERO_BOUND; /* Handle special case. */
    }

    return __builtin_ctzll(hash) + 1; /* Use builtin if possible. */
}

/* ================== Dense representation implementation  ================== */

/* Low level function to set the dense HLL register at 'index' to the
 * specified value if the current value is smaller than 'count'.
 *
 * 'registers' is expected to have room for HLL_REGISTERS plus an
 * additional byte on the right. This requirement is met by mdsc strings
 * automatically since they are implicitly null terminated.
 *
 * The function always succeed, however if as a result of the operation
 * the approximated cardinality changed, 1 is returned. Otherwise 0
 * is returned. */
DK_STATIC bool hyperloglogDenseSet(uint8_t *registers, int64_t index,
                                   uint_fast8_t count) {
    uint_fast8_t oldcount;

    /* Update the register if this element produced a longer run of zeroes. */
    HLL_DENSE_GET_REGISTER(oldcount, registers, index);
    if (count > oldcount) {
        HLL_DENSE_SET_REGISTER(registers, index, count);
        return true;
    }

    return false;
}

/* "Add" the element in the dense hyperloglog data structure.
 * Actually nothing is added, but the max 0 pattern counter of the subset
 * the element belongs to is incremented if needed.
 *
 * This is just a wrapper to hllDenseSet(), performing the hashing of the
 * element in order to retrieve the index and zero-run count. */
DK_STATIC bool hyperloglogDenseAdd(uint8_t *registers, const void *data_,
                                   size_t len) {
    int64_t index;
    uint_fast8_t count = hyperloglogPatLen(data_, len, &index);
    /* Update the register if this element produced a longer run of zeroes. */
    return hyperloglogDenseSet(registers, index, count);
}

/* Compute the register histogram in the dense representation. */
DK_STATIC void hyperloglogDenseRegisterHistogram(uint8_t *registers,
                                                 int32_t *reghisto) {
    /* default is to use 16384 registers 6 bits each. The code works
     * with other values by modifying the defines, but for our target value
     * we take a faster path with unrolled loops. */
    if (HLL_REGISTERS == 16384 && HLL_BITS == 6) {
        uint8_t *r = registers;
        uint64_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13,
            r14, r15;
        for (uint_fast32_t j = 0; j < 1024; j++) {
            r0 = r[0] & 63;
            r1 = (r[0] >> 6 | r[1] << 2) & 63;
            r2 = (r[1] >> 4 | r[2] << 4) & 63;
            r3 = (r[2] >> 2) & 63;
            r4 = r[3] & 63;
            r5 = (r[3] >> 6 | r[4] << 2) & 63;
            r6 = (r[4] >> 4 | r[5] << 4) & 63;
            r7 = (r[5] >> 2) & 63;
            r8 = r[6] & 63;
            r9 = (r[6] >> 6 | r[7] << 2) & 63;
            r10 = (r[7] >> 4 | r[8] << 4) & 63;
            r11 = (r[8] >> 2) & 63;
            r12 = r[9] & 63;
            r13 = (r[9] >> 6 | r[10] << 2) & 63;
            r14 = (r[10] >> 4 | r[11] << 4) & 63;
            r15 = (r[11] >> 2) & 63;

            reghisto[r0]++;
            reghisto[r1]++;
            reghisto[r2]++;
            reghisto[r3]++;
            reghisto[r4]++;
            reghisto[r5]++;
            reghisto[r6]++;
            reghisto[r7]++;
            reghisto[r8]++;
            reghisto[r9]++;
            reghisto[r10]++;
            reghisto[r11]++;
            reghisto[r12]++;
            reghisto[r13]++;
            reghisto[r14]++;
            reghisto[r15]++;

            r += 12;
        }
    } else {
        for (uint_fast32_t j = 0; j < HLL_REGISTERS; j++) {
            uint64_t reg;

            HLL_DENSE_GET_REGISTER(reg, registers, j);
            reghisto[reg]++;
        }
    }
}

/* ================== Sparse representation implementation  ================= */

/* Convert the HLL in sparse representation to dense representation.
 * Both representations are represented by MDSC strings, and
 * the input representation is freed as a side effect.
 *
 * The function returns true if the sparse representation was valid,
 * otherwise false is returned if the representation was corrupted. */
DK_STATIC bool hyperloglogSparseToDense(hyperloglog **inHll) {
    hyperloglog *h = *inHll;
    mdsc *sparse = toMDSC(h);
    mdsc *dense;
    hyperloglogHeader *hdr;
    hyperloglogHeader *oldhdr = h;
    int32_t idx = 0;
    int32_t runlen;
    int32_t regval;
    uint8_t *p = (uint8_t *)sparse;
    uint8_t *end = p + mdsclen(sparse);

    /* If the representation is already the right one return ASAP. */
    hdr = (hyperloglogHeader *)sparse;
    if (isDense(hdr)) {
        return true;
    }

    /* New string of the right size filled with zero bytes.
     * Note that the cached cardinality is set to 0 as a side effect
     * that is exactly the cardinality of an empty HLL. */
    dense = mdscnewlen(NULL, HLL_DENSE_SIZE);
    hdr = (hyperloglog *)dense;
    *hdr = *oldhdr; /* copies cached card */
    hdr->encoding = HLL_DENSE;

    /* Now read the sparse representation and set non-zero registers
     * accordingly. */
    p += HLL_HDR_SIZE;
    while (p < end) {
        if (HLL_SPARSE_IS_ZERO(p)) {
            runlen = HLL_SPARSE_ZERO_LEN(p);
            idx += runlen;
            p++;
        } else if (HLL_SPARSE_IS_XZERO(p)) {
            runlen = HLL_SPARSE_XZERO_LEN(p);
            idx += runlen;
            p += 2;
        } else {
            runlen = HLL_SPARSE_VAL_LEN(p);
            regval = HLL_SPARSE_VAL_VALUE(p);

            /* too big, corrupt */
            if ((runlen + idx) > HLL_REGISTERS) {
                return false;
            }

            while (runlen--) {
                HLL_DENSE_SET_REGISTER(hdr->registers, idx, regval);
                idx++;
            }

            p++;
        }
    }

    /* If the sparse representation was valid, we expect to find idx
     * set to HLL_REGISTERS. */
    if (idx != HLL_REGISTERS) {
        mdscfree(dense);
        assert(NULL && "Conversion error?");
        return false;
    }

    /* Free original representation and set the new one. */
    hyperloglogFree(*inHll);
    *inHll = toHLL(dense);

    return true;
}

/* "Add" the element in the sparse hyperloglog data structure.
 * Actually nothing is added, but the max 0 pattern counter of the subset
 * the element belongs to is incremented if needed.
 *
 * The object 'o' is the String object holding the HLL. The function requires
 * a reference to the object in order to be able to enlarge the string if
 * needed.
 *
 * On success, the function returns 1 if the cardinality changed, or 0
 * if the register for this element was not updated.
 * On error (if the representation is invalid) -1 is returned.
 *
 * As a side effect the function may promote the HLL representation from
 * sparse to dense: this happens when a register requires to be set to a value
 * not representable with the sparse representation, or when the resulting
 * size would be greater than server.hyperloglogSparseMaxBytes. */
DK_STATIC int32_t hyperloglogSparseSet(hyperloglog **inHll, int64_t index,
                                       uint_fast8_t count) {
    hyperloglog *h = *inHll;
    mdsc *hmdsc = toMDSC(h);
    hyperloglogHeader *hdr;
    uint_fast8_t oldcount;
    uint8_t *sparse;
    uint8_t *end;
    uint8_t *p;
    uint8_t *prev;
    uint8_t *next;
    int64_t first, span;
    int64_t isZero = 0, isXzero = 0, isVal = 0, runlen = 0;

    /* If the count is too big to be representable by the sparse representation
     * switch to dense representation. */
    if (count > HLL_SPARSE_VAL_MAX_VALUE) {
        goto promote;
    }

    /* When updating a sparse representation, sometimes we may need to
     * enlarge the buffer for up to 3 bytes in the worst case (XZERO split
     * into XZERO-va_l-XZERO). Make sure there is enough space right now
     * so that the pointers we take during the execution of the function
     * will be valid all the time. */
    hmdsc = mdscexpandby(hmdsc, 3);
    *inHll = toHLL(hmdsc);

    /* Step 1: we need to locate the opcode we need to modify to check
     * if a value update is actually needed. */
    sparse = p = ((uint8_t *)hmdsc) + HLL_HDR_SIZE;
    end = p + mdsclen(hmdsc) - HLL_HDR_SIZE;

    first = 0;
    prev = NULL; /* Points to previous opcode at the end of the loop. */
    next = NULL; /* Points to the next opcode at the end of the loop. */
    span = 0;
    while (p < end) {
        int64_t oplen;

        /* Set span to the number of registers covered by this opcode.
         *
         * This is the most performance critical loop of the sparse
         * representation. Sorting the conditionals from the most to the
         * least frequent opcode in many-bytes sparse HLLs is faster. */
        oplen = 1;
        if (HLL_SPARSE_IS_ZERO(p)) {
            span = HLL_SPARSE_ZERO_LEN(p);
        } else if (HLL_SPARSE_IS_VAL(p)) {
            span = HLL_SPARSE_VAL_LEN(p);
        } else { /* XZERO. */
            span = HLL_SPARSE_XZERO_LEN(p);
            oplen = 2;
        }
        /* Break if this opcode covers the register as 'index'. */
        if (index <= first + span - 1) {
            break;
        }

        prev = p;
        p += oplen;
        first += span;
    }
    if (span == 0) {
        return -1; /* Invalid format. */
    }

    next = HLL_SPARSE_IS_XZERO(p) ? p + 2 : p + 1;
    if (next >= end) {
        next = NULL;
    }

    /* Cache current opcode type to avoid using the macro again and
     * again for something that will not change.
     * Also cache the run-length of the opcode. */
    if (HLL_SPARSE_IS_ZERO(p)) {
        isZero = 1;
        runlen = HLL_SPARSE_ZERO_LEN(p);
    } else if (HLL_SPARSE_IS_XZERO(p)) {
        isXzero = 1;
        runlen = HLL_SPARSE_XZERO_LEN(p);
    } else {
        isVal = 1;
        runlen = HLL_SPARSE_VAL_LEN(p);
    }

    /* Step 2: After the loop:
     *
     * 'first' stores to the index of the first register covered
     *  by the current opcode, which is pointed by 'p'.
     *
     * 'next' ad 'prev' store respectively the next and previous opcode,
     *  or NULL if the opcode at 'p' is respectively the last or first.
     *
     * 'span' is set to the number of registers covered by the current
     *  opcode.
     *
     * There are different cases in order to update the data structure
     * in place without generating it from scratch:
     *
     * A) If it is a VAL opcode already set to a value >= our 'count'
     *    no update is needed, regardless of the VAL run-length field.
     *    In this case PFADD returns 0 since no changes are performed.
     *
     * B) If it is a VAL opcode with len = 1 (representing only our
     *    register) and the value is less than 'count', we just update it
     *    since this is a trivial case. */
    if (isVal) {
        oldcount = HLL_SPARSE_VAL_VALUE(p);
        /* Case A. */
        if (oldcount >= count) {
            return 0;
        }

        /* Case B. */
        if (runlen == 1) {
            HLL_SPARSE_VAL_SET(p, count, 1);
            goto updated;
        }
    }

    /* C) Another trivial to handle case is a ZERO opcode with a len of 1.
     * We can just replace it with a VAL opcode with our value and len of 1. */
    if (isZero && runlen == 1) {
        HLL_SPARSE_VAL_SET(p, count, 1);
        goto updated;
    }

    /* D) General case.
     *
     * The other cases are more complex: our register requires to be updated
     * and is either currently represented by a VAL opcode with len > 1,
     * by a ZERO opcode with len > 1, or by an XZERO opcode.
     *
     * In those cases the original opcode must be split into multiple
     * opcodes. The worst case is an XZERO split in the middle resuling into
     * XZERO - VAL - XZERO, so the resulting sequence max length is
     * 5 bytes.
     *
     * We perform the split writing the new sequence into the 'new' buffer
     * with 'newlen' as length. Later the new sequence is inserted in place
     * of the old one, possibly moving what is on the right a few bytes
     * if the new sequence is longer than the older one. */
    uint8_t seq[5];
    uint8_t *n = seq;
    int32_t last =
        first + span - 1; /* Last register covered by the sequence. */
    int32_t len;

    if (isZero || isXzero) {
        /* Handle splitting of ZERO / XZERO. */
        if (index != first) {
            len = index - first;
            if (len > HLL_SPARSE_ZERO_MAX_LEN) {
                HLL_SPARSE_XZERO_SET(n, len);
                n += 2;
            } else {
                HLL_SPARSE_ZERO_SET(n, len);
                n++;
            }
        }
        HLL_SPARSE_VAL_SET(n, count, 1);
        n++;
        if (index != last) {
            len = last - index;
            if (len > HLL_SPARSE_ZERO_MAX_LEN) {
                HLL_SPARSE_XZERO_SET(n, len);
                n += 2;
            } else {
                HLL_SPARSE_ZERO_SET(n, len);
                n++;
            }
        }
    } else {
        /* Handle splitting of va_l. */
        const int32_t curval = HLL_SPARSE_VAL_VALUE(p);

        if (index != first) {
            len = index - first;
            HLL_SPARSE_VAL_SET(n, curval, len);
            n++;
        }
        HLL_SPARSE_VAL_SET(n, count, 1);
        n++;
        if (index != last) {
            len = last - index;
            HLL_SPARSE_VAL_SET(n, curval, len);
            n++;
        }
    }

    /* Step 3: substitute the new sequence with the old one.
     *
     * Note that we already allocated space on the mdsc string
     * calling mdscMakeRoomFor(). */
    const int32_t seqlen = n - seq;
    const int32_t oldlen = isXzero ? 2 : 1;
    const int32_t deltalen = seqlen - oldlen;

    if (deltalen > 0 && mdsclen(hmdsc) + deltalen > serverSparseMaxBytes) {
        goto promote;
    }

    if (deltalen && next) {
        memmove(next + deltalen, next, end - next);
    }

    mdscIncrLen(hmdsc, deltalen);
    memcpy(p, seq, seqlen);
    end += deltalen;

updated:
    /* Step 4: Merge adjacent values if possible.
     *
     * The representation was updated, however the resulting representation
     * may not be optimal: adjacent VAL opcodes can sometimes be merged into
     * a single one. */
    p = prev ? prev : sparse;
    int32_t scanlen = 5; /* Scan up to 5 upcodes starting from prev. */
    while (p < end && scanlen--) {
        if (HLL_SPARSE_IS_XZERO(p)) {
            p += 2;
            continue;
        } else if (HLL_SPARSE_IS_ZERO(p)) {
            p++;
            continue;
        }
        /* We need two adjacent VAL opcodes to try a merge, having
         * the same value, and a len that fits the VAL opcode max len. */
        if (p + 1 < end && HLL_SPARSE_IS_VAL(p + 1)) {
            const int32_t v1 = HLL_SPARSE_VAL_VALUE(p);
            const int32_t v2 = HLL_SPARSE_VAL_VALUE(p + 1);
            if (v1 == v2) {
                const int32_t combinedLen =
                    HLL_SPARSE_VAL_LEN(p) + HLL_SPARSE_VAL_LEN(p + 1);
                if (combinedLen <= HLL_SPARSE_VAL_MAX_LEN) {
                    HLL_SPARSE_VAL_SET(p + 1, v1, combinedLen);
                    memmove(p, p + 1, end - p);
                    mdscIncrLen(hmdsc, -1);
                    end--;
                    /* After a merge we reiterate without incrementing 'p'
                     * in order to try to merge the just merged value with
                     * a value on its right. */
                    continue;
                }
            }
        }
        p++;
    }

    /* Invalidate the cached cardinality. */
    hdr = toHLL(hmdsc);
    HLL_INVALIDATE_CACHE(hdr);
    *inHll = hdr;
    return 1;

promote: /* Promote to dense representation. */
    if (hyperloglogSparseToDense(toHLLIn(hmdsc)) == false) {
        assert(NULL && "No convert?");
    }

    hdr = toHLL(hmdsc);
    *inHll = hdr;

    /* We need to call hyperloglogDenseAdd() to perform the operation after the
     * conversion. However the result must be 1, since if we need to
     * convert from sparse to dense a register requires to be updated.
     *
     * Note that this in turn means that PFADD will make sure the command
     * is propagated to slaves / AOF, so if there is a sparse -> dense
     * conversion, it will be performed in all the slaves as well. */
    const int32_t denseRetval =
        hyperloglogDenseSet(hdr->registers, index, count);
    assert(denseRetval == 1);
    return denseRetval;
}

/* Low level function to set the sparse HLL register at 'index' to the
 * specified value if the current value is smaller than 'count'.
 *
 * This function is actually a wrapper for hllSparseSet(), it only performs
 * the hashshing of the elmenet to obtain the index and zeros run length. */
DK_STATIC int32_t hyperloglogSparseAdd(hyperloglog **inHll, const void *data_,
                                       size_t len) {
    int64_t index;
    const uint_fast8_t count = hyperloglogPatLen(data_, len, &index);

    /* Update the register if this element produced a longer run of zeroes. */
    return hyperloglogSparseSet(inHll, index, count);
}

/* Compute the register histogram in the sparse representation. */
DK_STATIC void hyperloglogSparseRegisterHistogram(uint8_t *sparse,
                                                  int sparselen, bool *invalid,
                                                  int32_t *reghisto) {
    int32_t idx = 0;
    int32_t runlen;
    int32_t regval;
    uint8_t *end = sparse + sparselen;
    uint8_t *p = sparse;

    while (p < end) {
        if (HLL_SPARSE_IS_ZERO(p)) {
            runlen = HLL_SPARSE_ZERO_LEN(p);
            idx += runlen;
            reghisto[0] += runlen;
            p++;
        } else if (HLL_SPARSE_IS_XZERO(p)) {
            runlen = HLL_SPARSE_XZERO_LEN(p);
            idx += runlen;
            reghisto[0] += runlen;
            p += 2;
        } else {
            runlen = HLL_SPARSE_VAL_LEN(p);
            regval = HLL_SPARSE_VAL_VALUE(p);
            idx += runlen;
            reghisto[regval] += runlen;
            p++;
        }
    }

    if (idx != HLL_REGISTERS && invalid) {
        *invalid = true;
    }
}

/* ========================= HyperLogLog Count ==============================
 * This is the core of algorithm where the approximated count is computed. */
/* The function uses the lower level hllDenseRegHisto() and hllSparseRegHisto()
 * functions as helpers to compute histogram of register values part of the
 * computation, representation-specific, while all the rest is common. */

/* Implements the register histogram calculation for uint8_t data type
 * which is only used internally as speedup for PFCOUNT with multiple keys. */
void hyperloglogRawRegisterHistogram(uint8_t *registers, int32_t *reghisto) {
    const uint64_t *word = (uint64_t *)registers;
    uint8_t *bytes;

    for (int_fast32_t j = 0; j < HLL_REGISTERS / 8; j++) {
        if (*word == 0) {
            reghisto[0] += 8;
        } else {
            bytes = (uint8_t *)word;
            reghisto[bytes[0]]++;
            reghisto[bytes[1]]++;
            reghisto[bytes[2]]++;
            reghisto[bytes[3]]++;
            reghisto[bytes[4]]++;
            reghisto[bytes[5]]++;
            reghisto[bytes[6]]++;
            reghisto[bytes[7]]++;
        }
        word++;
    }
}

/* Helper function sigma as defined in
 * "New cardinality estimation algorithms for HyperLogLog sketches"
 * Otmar Ertl, arXiv:1702.01284 */
static DK_FN_PURE double hllSigma(double x) {
    if (x == 1.) {
        return INFINITY;
    }

    double zPrime;
    double y = 1;
    double z = x;
    do {
        x *= x;
        zPrime = z;
        z += x * y;
        y += y;
    } while (zPrime != z);

    return z;
}

/* Helper function tau as defined in
 * "New cardinality estimation algorithms for HyperLogLog sketches"
 * Otmar Ertl, arXiv:1702.01284 */
static DK_FN_PURE double hllTau(double x) {
    if (x == 0. || x == 1.) {
        return 0.;
    }

    double zPrime;
    double y = 1.0;
    double z = 1 - x;

#if __AVX__
    /* Clear the upper half of AVX registers before calling pow(3).
     * This can save thousands of AVX-to-SSE transitions and speeds up the
     * following loop about 10% vs. not-clearing. */

    /* See threads like:
     * https://software.intel.com/en-us/forums/intel-isa-extensions/topic/704023
     */
    _mm256_zeroupper();
#endif

    do {
        x = sqrt(x);
        zPrime = z;
        y *= 0.5;
        z -= pow(1 - x, 2) * y;
    } while (zPrime != z);

    return z / 3;
}

/* Return the approximated cardinality of the set based on the harmonic
 * mean of the registers values. 'hdr' points to the start of the string
 * representing the String object holding the HLL representation.
 *
 * If the sparse representation of the HLL object is not valid, the integer
 * pointed by 'invalid' is set to non-zero, otherwise it is left untouched.
 *
 * hyperloglogCount() supports a special internal-only encoding of HLL_RAW, that
 * is, hdr->registers will point to an uint8_t array of HLL_REGISTERS element.
 * This is useful in order to speedup PFCOUNT when called against multiple
 * keys (no need to work with 6-bit integers encoding). */
DK_INLINE_ALWAYS uint64_t hyperloglogCount_(hyperloglog *hdr, bool *invalid) {
    double m = HLL_REGISTERS;
    double E;
    int32_t reghisto[HLL_Q + 2] = {0};

    /* Compute register histogram */
    if (isDense(hdr)) {
        hyperloglogDenseRegisterHistogram(hdr->registers, reghisto);
    } else if (isSparse(hdr)) {
        hyperloglogSparseRegisterHistogram(hdr->registers,
                                           mdsclen((mdsc *)hdr) - HLL_HDR_SIZE,
                                           invalid, reghisto);
    } else if (hdr->encoding == HLL_RAW) {
        hyperloglogRawRegisterHistogram(hdr->registers, reghisto);
    } else {
        /* Error condition! */
        return -1; /* this is BAD because return becomes UINT64_MAX */
    }

    /* Estimate cardinality form register histogram. See:
     * "New cardinality estimation algorithms for HyperLogLog sketches"
     * Otmar Ertl, arXiv:1702.01284 */
    double z = m * hllTau((m - reghisto[HLL_Q + 1]) / (double)m);
    for (int_fast32_t j = HLL_Q; j >= 1; --j) {
        z += reghisto[j];
        z *= 0.5;
    }

    z += m * hllSigma(reghisto[0] / (double)m);
    E = llroundl(HLL_ALPHA_INF * m * m / z);

    return (uint64_t)E;
}

uint64_t hyperloglogCount(hyperloglog *hdr, bool *invalid) {
    return hyperloglogCount_(hdr, invalid);
}

/* Call hyperloglogDenseAdd() or hyperloglogSparseAdd() according to the HLL
 * encoding. */
int hyperloglogAdd(hyperloglog **inHll, const void *data, size_t size) {
    hyperloglogHeader *hdr = *inHll;
    if (hdr->encoding == HLL_DENSE) {
        return hyperloglogDenseAdd(hdr->registers, data, size);
    }

    /* else, is sparse */
    return hyperloglogSparseAdd(inHll, data, size);
}

/* Merge by computing MAX(registers[i],hyperloglog[i]) the HyperLogLog
 * 'hyperloglog' with an array of uint8_t HLL_REGISTERS registers
 * pointed by 'max'.
 *
 * The hyperloglog object must be valid (either by hyperloglogDetect() or some
 * other way).
 *
 * If the HyperLogLog is sparse and is found to be invalid, false
 * is returned, otherwise the function always succeeds. */
bool hyperloglogMerge(hyperloglog *restrict target,
                      const hyperloglog *restrict hdr) {
    uint8_t *max = target->registers;

    /* Target must use 8-bit per register (HLL_RAW) for direct array access.
     * Source should be DENSE or SPARSE, not RAW. */
    assert(target->encoding == HLL_RAW);
    assert(hdr->encoding != HLL_RAW);

    if (isDense(hdr)) {
#if HLL_USE_NEON && HLL_REGISTERS == 16384 && HLL_BITS == 6
        /* ARM NEON optimized merge: process 16 registers (12 packed bytes) at a
         * time. Unpacks 6-bit registers to 8-bit, then uses vector max. */
        uint8_t *r = (uint8_t *)hdr->registers;
        for (size_t j = 0; j < 1024; j++) {
            /* Unpack 16 6-bit registers from 12 bytes into temporary buffer */
            uint8_t unpacked[16];
            unpacked[0] = r[0] & 63;
            unpacked[1] = (r[0] >> 6 | r[1] << 2) & 63;
            unpacked[2] = (r[1] >> 4 | r[2] << 4) & 63;
            unpacked[3] = (r[2] >> 2) & 63;
            unpacked[4] = r[3] & 63;
            unpacked[5] = (r[3] >> 6 | r[4] << 2) & 63;
            unpacked[6] = (r[4] >> 4 | r[5] << 4) & 63;
            unpacked[7] = (r[5] >> 2) & 63;
            unpacked[8] = r[6] & 63;
            unpacked[9] = (r[6] >> 6 | r[7] << 2) & 63;
            unpacked[10] = (r[7] >> 4 | r[8] << 4) & 63;
            unpacked[11] = (r[8] >> 2) & 63;
            unpacked[12] = r[9] & 63;
            unpacked[13] = (r[9] >> 6 | r[10] << 2) & 63;
            unpacked[14] = (r[10] >> 4 | r[11] << 4) & 63;
            unpacked[15] = (r[11] >> 2) & 63;

            /* NEON vector max operation - 16 bytes at once */
            uint8x16_t src = vld1q_u8(unpacked);
            uint8x16_t dst = vld1q_u8(max);
            uint8x16_t result = vmaxq_u8(src, dst);
            vst1q_u8(max, result);

            max += 16;
            r += 12;
        }
#elif HLL_USE_SSE && HLL_REGISTERS == 16384 && HLL_BITS == 6
        /* SSE optimized merge: same approach as NEON */
        uint8_t *r = (uint8_t *)hdr->registers;
        for (size_t j = 0; j < 1024; j++) {
            /* Unpack 16 6-bit registers from 12 bytes into temporary buffer */
            uint8_t unpacked[16];
            unpacked[0] = r[0] & 63;
            unpacked[1] = (r[0] >> 6 | r[1] << 2) & 63;
            unpacked[2] = (r[1] >> 4 | r[2] << 4) & 63;
            unpacked[3] = (r[2] >> 2) & 63;
            unpacked[4] = r[3] & 63;
            unpacked[5] = (r[3] >> 6 | r[4] << 2) & 63;
            unpacked[6] = (r[4] >> 4 | r[5] << 4) & 63;
            unpacked[7] = (r[5] >> 2) & 63;
            unpacked[8] = r[6] & 63;
            unpacked[9] = (r[6] >> 6 | r[7] << 2) & 63;
            unpacked[10] = (r[7] >> 4 | r[8] << 4) & 63;
            unpacked[11] = (r[8] >> 2) & 63;
            unpacked[12] = r[9] & 63;
            unpacked[13] = (r[9] >> 6 | r[10] << 2) & 63;
            unpacked[14] = (r[10] >> 4 | r[11] << 4) & 63;
            unpacked[15] = (r[11] >> 2) & 63;

            /* SSE vector max operation - 16 bytes at once */
            __m128i src = _mm_loadu_si128((__m128i *)unpacked);
            __m128i dst = _mm_loadu_si128((__m128i *)max);
            __m128i result = _mm_max_epu8(src, dst);
            _mm_storeu_si128((__m128i *)max, result);

            max += 16;
            r += 12;
        }
#else
        /* Scalar fallback */
        uint8_t val;
        for (size_t i = 0; i < HLL_REGISTERS; i++) {
            HLL_DENSE_GET_REGISTER(val, hdr->registers, i);
            if (val > max[i]) {
                max[i] = val;
            }
        }
#endif
    } else {
        uint8_t *p = (uint8_t *)hdr;
        uint8_t *end = p + mdsclen(toMDSC(hdr));
        int64_t runlen;
        int64_t regval;

        p += HLL_HDR_SIZE;
        int_fast32_t i = 0;
        while (p < end) {
            if (HLL_SPARSE_IS_ZERO(p)) {
                runlen = HLL_SPARSE_ZERO_LEN(p);
                i += runlen;
                p++;
            } else if (HLL_SPARSE_IS_XZERO(p)) {
                runlen = HLL_SPARSE_XZERO_LEN(p);
                i += runlen;
                p += 2;
            } else {
                runlen = HLL_SPARSE_VAL_LEN(p);
                regval = HLL_SPARSE_VAL_VALUE(p);

                /* too big, corrupt */
                if ((runlen + i) > HLL_REGISTERS) {
                    return false;
                }

                while (runlen--) {
                    if (regval > max[i]) {
                        max[i] = regval;
                    }

                    i++;
                }

                p++;
            }
        }

        if (i != HLL_REGISTERS) {
            return false;
        }
    }

    return true;
}

/* ========================== HyperLogLog commands ========================== */

/* New HLL object. We always create the HLL using sparse encoding.
 * This will be upgraded to the dense representation as needed. */
hyperloglog *hyperloglogNew(void) {
    const size_t sparselen =
        HLL_HDR_SIZE + (((HLL_REGISTERS + (HLL_SPARSE_XZERO_MAX_LEN - 1)) /
                         HLL_SPARSE_XZERO_MAX_LEN) *
                        2);

    /* Populate the sparse representation with as many XZERO opcodes as
     * needed to represent all the registers. */
    int_fast32_t aux = HLL_REGISTERS;

    const mdsc *s = mdscnewlen(NULL, sparselen);
    hyperloglog *restrict h = toHLL(s);

    uint8_t *p = (uint8_t *)h + HLL_HDR_SIZE;

    while (aux) {
        int_fast32_t xzero = HLL_SPARSE_XZERO_MAX_LEN;
        if (xzero > aux) {
            xzero = aux;
        }

        HLL_SPARSE_XZERO_SET(p, xzero);
        p += 2;
        aux -= xzero;
    }

    assert((p - (uint8_t *)s) == sparselen);

    /* Create the actual object. */
    h->encoding = HLL_SPARSE;
    return h;
}

hyperloglog *hyperloglogNewSparse(void) {
    return hyperloglogNew();
}

hyperloglog *hyperloglogNewDense(void) {
    hyperloglog *dense = toHLL(mdscnewlen(NULL, HLL_DENSE_SIZE));
    dense->encoding = HLL_DENSE;
    return dense;
}

void hyperloglogFree(hyperloglog *h) {
    if (h) {
        mdscfree(toMDSC(h));
    }
}

hyperloglog *hyperloglogCopy(const hyperloglog *src) {
    /* "raw" HLLs are static allocations, not MDSC strings! */
    assert(src->encoding != HLL_RAW);

    return toHLL(mdscnewlen(toMDSC(src), mdsclen(toMDSC(src))));
}

void hyperloglogInvalidateCache(hyperloglog *h) {
    HLL_INVALIDATE_CACHE(h);
}

/* Check if the object is a String with a valid HLL representation.
 * Return true if this is true, otherwise reply to the client
 * with an error and return false. */
bool hyperloglogDetect(hyperloglog *h) {
    size_t len = mdsclen(toMDSC(h));

    if (!h) {
        return false;
    }

    if (len < sizeof(*h)) {
        return false;
    }

    if (h->encoding > HLL_MAX_ENCODING) {
        return false;
    }

    /* Dense representation string length should match exactly. */
    if ((h->encoding == HLL_DENSE) && (len != HLL_DENSE_SIZE)) {
        return false;
    }

    /* All tests passed. */
    return true;
}

/* Add elements to HLL.
 * Parameters alternate: data, len, data2, len2, ..., NULL
 * Return value is number of elements updated in HLL */
int pfadd(hyperloglog **hh, ...) {
    hyperloglog *h = *hh;
    uint_fast32_t updated = 0;

    if (!h) {
        h = hyperloglogNew();
    }

    va_list ap;
    uint8_t *argData = 0;
    uintptr_t argLen = 0;

    va_start(ap, hh);
    /* Perform the low level ADD operation for every element. */
    for (int_fast32_t loopr = 0;; loopr++) {
        if (loopr % 2 == 0) {
            /* If we are on an even entry, add data using len */
            if (hyperloglogAdd(&h, argData, argLen)) {
                updated++;
            } else {
                va_end(ap);
                return false;
            }
            /* If the next data is NULL, we have reached the end of args. */
            if ((argData = va_arg(ap, uint8_t *)) == NULL) {
                break;
            }
        } else {
            /* If we are on an odd entry, just populate len.
             * We are cheating the type system by storing
             * the length as a direct pointer value then
             * casting back to an integer type. */
            argLen = va_arg(ap, uintptr_t);
        }
    }
    va_end(ap);

    if (updated) {
        HLL_INVALIDATE_CACHE(h);
    }

    *hh = h;
    return updated;
}

/* PFCOUNT var -> approximated cardinality of set. */
uint64_t pfcountSingle(hyperloglog *h) {
    uint64_t card;
    /* Cardinality of the single HLL.
     *
     * The user specified a single key. Either return the cached value
     * or compute one and update the cache. */

#if 0
    if (hyperloglogDetect(h) == false) {
        return -1;
    }
#endif

    /* Check if the cached cardinality is valid. */
    hyperloglog *hdr = h;
    if (HLL_VALID_CACHE(hdr)) {
        /* Just return the cached value. */
        card = hdr->cardinality;
    } else {
        bool invalid = false;
        /* Recompute it and update the cached value. */
        card = hyperloglogCount(hdr, &invalid);
        if (invalid) {
            return -1;
        }

        if (card < HLL_CARDINALITY_MAX) {
            /* You've got bigger problems if your count
             * reaches 2^61 - 1... */
            hdr->cardinality = card;
            hdr->cardinalityValid = true;
        }

        /* Note: this read can modify the HLL because it updates
         * the cached cardinality. It's a policy decision whether
         * this cache update should constitute a new cross-node
         * persisting wite. */
    }

    return card;
}

DK_INLINE_ALWAYS uint64_t pfvcount(hyperloglog *first, va_list ap) {
#if 0
    hyperloglog *second = va_arg(ap, hyperloglog *);
    if (!second) {
        return pfcountSingle(first);
    }
#endif

    /* multi-key keys, cardinality of the union.
     *
     * When multiple keys are specified, PFCOUNT actually computes
     * the cardinality of the merge of the N HLLs specified. */
    hllStatic max = {0};

    /* Compute an HLL with M[i] = MAX(M[i]_j). */
    hyperloglog *hdr = toHLL(max);
    hdr->encoding = HLL_RAW; /* Special internal-only encoding. */

    HLL_MERGE(hdr, first, -1);
#if 0
    HLL_MERGE(hdr, second, -1);
#endif

    hyperloglog *current;
    while ((current = va_arg(ap, hyperloglog *))) {
        HLL_MERGE(hdr, current, -1);
    }

    /* Compute cardinality of the resulting set. */
    return hyperloglogCount_(hdr, NULL);
}

/* Returns merged count of all input parameters. */
uint64_t pfcount(hyperloglog *h, ...) {
    va_list ap;
    va_start(ap, h);
    const uint64_t count = pfvcount(h, ap);
    va_end(ap);
    return count;
}

/* PFMERGE src1 src2 src3 ... srcN NULL => merged HLL */
DK_INLINE_ALWAYS hyperloglog *pfvmerge(hyperloglog *first, va_list ap) {
    hllStatic max = {0};
    hyperloglog *restrict total = toHLL(max);
    bool useDense = false;

    HLL_MERGE(total, first, NULL);
    /* Compute an HLL with M[i] = MAX(M[i]_j).
     * We we the maximum into the max array of registers. We'll write
     * it to the target variable later. */
    hyperloglog *current;
    while ((current = va_arg(ap, hyperloglog *))) {
        HLL_MERGE(total, current, NULL);
        if (current->encoding == HLL_DENSE) {
            useDense = true;
        }
    }

    /* Create / unshare the destination key's value if needed. */
    hyperloglog *result = hyperloglogNew();

    /* Convert the destination object to dense representation if at least
     * one of the inputs was dense. */
    if (useDense && !hyperloglogSparseToDense(&result)) {
        return NULL;
    }

    /* Write the resulting HLL to the destination HLL registers and
     * invalidate the cached value. */
    for (int j = 0; j < HLL_REGISTERS; j++) {
        if (max[j] == 0) {
            continue;
        }

        if (result->encoding == HLL_DENSE) {
            hyperloglogDenseSet(result->registers, j, max[j]);
        } else {
            /* else, HLL_SPARSE */
            hyperloglogSparseSet(&result, j, max[j]);
        }
    }

    HLL_INVALIDATE_CACHE(result);

    return result;
}

/* Returns newly allocated hyperloglog having cardinality of all inputs. */
hyperloglog *pfmerge(hyperloglog *h, ...) {
    va_list ap;
    va_start(ap, h);
    hyperloglog *restrict const merged = pfvmerge(h, ap);
    va_end(ap);
    return merged;
}

/* ========================== Testing / Debugging  ========================== */

#ifdef DATAKIT_TEST
#include "datakit.h"
#include "timeUtil.h"

/* PFSELFTEST
 * This command performs a self-test of the HLL registers implementation.
 * Something that is not easy to test from within the outside. */
#define HLL_TEST_CYCLES 100000
int hyperloglogTest(int argc, char *argv[]) {
    DK_NOTUSED(argc);
    DK_NOTUSED(argv);

    uint32_t sseed = time(NULL) ^ getpid();
    uint32_t shash = HYPERHASH(&sseed, sizeof(sseed), 0xadc83b19ULL);
    srand(shash);

    printf("Testing (using seed: %" PRIu32 ") for %" PRId32 " cycles\n",
           (uint32_t)shash, (int32_t)HLL_TEST_CYCLES);

    mdsc *bitcounters = mdscnewlen(NULL, HLL_DENSE_SIZE);
    hyperloglogHeader *hdr = (hyperloglogHeader *)bitcounters, *hdr2;
    uint8_t bytecounters[HLL_REGISTERS] = {0};
    int32_t errors = 0;
    hyperloglog *h = NULL;

    /* Test 1: access registers.
     * The test is conceived to test that the different counters of our data
     * structure are accessible and that setting their values both result in
     * the correct value to be retained and not affect adjacent values. */
    for (uint32_t j = 0; j < HLL_TEST_CYCLES; j++) {
        if (j % 5000 == 0) {
            printf("[register access]: Testing cycle %" PRIu32
                   " (%0.2f%% done)\n",
                   j, (float)j / HLL_TEST_CYCLES * 100);
        }
        /* Set the HLL counters and an array of unsigned byes of the
         * same size to the same set of random values. */
        for (uint32_t i = 0; i < HLL_REGISTERS; i++) {
            uint32_t r = rand() & HLL_REGISTER_MAX;

            bytecounters[i] = r;
            HLL_DENSE_SET_REGISTER(hdr->registers, i, r);
        }

        /* Check that we are able to retrieve the same values. */
        for (uint32_t i = 0; i < HLL_REGISTERS; i++) {
            uint32_t val;

            HLL_DENSE_GET_REGISTER(val, hdr->registers, i);
            if (val != bytecounters[i]) {
                printf("TESTFAILED Register %" PRIu32 " should be %" PRIu8
                       " but is %" PRIu32 "\n",
                       (uint32_t)i, (uint8_t)bytecounters[i], (uint32_t)val);
                errors++;
                goto cleanup;
            }
        }
    }

    /* Test 2: approximation error.
     * The test adds unique elements and check that the estimated value
     * is always reasonable bounds.
     *
     * We check that the error is smaller than a few times than the expected
     * standard error, to make it very unlikely for the test to fail because
     * of a "bad" run.
     *
     * The test is performed with both dense and sparse HLLs at the same
     * time also verifying that the computed cardinality is the same. */
    memset(hdr->registers, 0, HLL_DENSE_SIZE - HLL_HDR_SIZE);
    h = hyperloglogNew();
    double relerr = 1.04 / sqrt(HLL_REGISTERS);
    uint64_t checkpoint = 1;
    uint64_t seed = (uint64_t)rand() | (uint64_t)rand() << 32;
    uint64_t ele;
    uint64_t testCount = HLL_TEST_CYCLES * 10000;
    for (uint64_t j = 1; j <= testCount; j++) {
        if (j % 500000 == 0) {
            printf("[approximation error]: Testing cycle %" PRIu64
                   " (%0.2f%% done)\n",
                   j, (float)j / testCount * 100);
        }
        ele = j ^ seed;
        hyperloglogDenseAdd(hdr->registers, (uint8_t *)&ele, sizeof(ele));
        hyperloglogAdd(&h, &ele, sizeof(ele));

        /* Make sure for small cardinalities we use sparse encoding. */
        if (j == checkpoint && j < serverSparseMaxBytes / 2) {
            hdr2 = h;
            if (hdr2->encoding != HLL_SPARSE) {
                printf("TESTFAILED sparse encoding not used\n");
                errors++;
                goto cleanup;
            }
        }

        /* Check that dense and sparse representations agree. */
        if (j == checkpoint &&
            hyperloglogCount(hdr, NULL) != hyperloglogCount(h, NULL)) {
            printf("TESTFAILED dense/sparse disagree\n");
            errors++;
            goto cleanup;
        }

        /* Check error. */
        if (j == checkpoint) {
            int64_t abserr = checkpoint - (int64_t)hyperloglogCount(hdr, NULL);
            uint64_t maxerr = ceil(relerr * 6 * checkpoint);

            /* Adjust the max error we expect for cardinality 10
             * since from time to time it is statistically likely to get
             * much higher error due to collision, resulting into a false
             * positive. */
            if (j == 10) {
                maxerr = 1;
            }

            if (abserr < 0) {
                abserr = -abserr;
            }

            if (abserr > (int64_t)maxerr) {
                printf("TESTFAILED Too big error. card:%" PRIu64
                       " abserr:%" PRIu64 "\n",
                       checkpoint, abserr);
                errors++;
                goto cleanup;
            }
            checkpoint *= 10;
        }
    }

    /* Success! */
    printf("ALL TESTS PASSED!\n");

cleanup:
    mdscfree(bitcounters);
    if (h) {
        hyperloglogFree(h);
    }
    if (errors) {
        printf("TESTS FAILED!  Error count: %d\n", errors);
    }

    /* Test 3: SIMD merge correctness.
     * Verify that merging dense HLLs produces correct results. */
    printf("[merge correctness]: Testing SIMD merge...\n");
    {
        hyperloglog *h1 = hyperloglogNewDense();
        hyperloglog *h2 = hyperloglogNewDense();

        /* Add different elements to each HLL */
        for (uint64_t i = 0; i < 10000; i++) {
            uint64_t v1 = i * 2;
            uint64_t v2 = i * 2 + 1;
            hyperloglogAdd(&h1, &v1, sizeof(v1));
            hyperloglogAdd(&h2, &v2, sizeof(v2));
        }

        uint64_t count1 = pfcountSingle(h1);
        uint64_t count2 = pfcountSingle(h2);

        /* Merge h2 into a new HLL that also has h1's data */
        hyperloglog *merged = pfmerge(h1, h2, NULL);
        uint64_t countMerged = pfcountSingle(merged);

        /* Merged count should be approximately count1 + count2 */
        /* Allow 10% error margin */
        uint64_t expectedMin = (count1 + count2) * 90 / 100;
        uint64_t expectedMax = (count1 + count2) * 110 / 100;

        if (countMerged < expectedMin || countMerged > expectedMax) {
            printf("TESTFAILED merge count %" PRIu64 " not in range [%" PRIu64
                   ", %" PRIu64 "] (h1=%" PRIu64 ", h2=%" PRIu64 ")\n",
                   countMerged, expectedMin, expectedMax, count1, count2);
            errors++;
        } else {
            printf("[merge correctness]: PASSED (merged=%" PRIu64
                   ", expected ~%" PRIu64 ")\n",
                   countMerged, count1 + count2);
        }

        hyperloglogFree(h1);
        hyperloglogFree(h2);
        hyperloglogFree(merged);
    }

    /* Test 4: SIMD merge performance benchmark.
     * Measures merge throughput with SIMD optimizations. */
    printf("[merge benchmark]: Benchmarking SIMD merge performance...\n");
    {
        const int numHlls = 100;
        const int mergeIterations = 1000;

        /* Create test HLLs with data */
        hyperloglog *hlls[numHlls];
        for (int i = 0; i < numHlls; i++) {
            hlls[i] = hyperloglogNewDense();
            for (uint64_t j = 0; j < 1000; j++) {
                uint64_t v = i * 1000 + j;
                hyperloglogAdd(&hlls[i], &v, sizeof(v));
            }
        }

        /* Benchmark merge operations */
        uint64_t startTime = timeUtilMonotonicNs();

        for (int iter = 0; iter < mergeIterations; iter++) {
            hyperloglog *result = pfmerge(hlls[0], hlls[1], NULL);
            for (int i = 2; i < numHlls; i++) {
                hyperloglog *newResult = pfmerge(result, hlls[i], NULL);
                hyperloglogFree(result);
                result = newResult;
            }
            hyperloglogFree(result);
        }

        uint64_t endTime = timeUtilMonotonicNs();
        uint64_t totalNs = endTime - startTime;
        uint64_t totalMerges = (uint64_t)mergeIterations * (numHlls - 1);
        double nsPerMerge = (double)totalNs / totalMerges;
        double mergesPerSec = 1e9 / nsPerMerge;

        printf("[merge benchmark]: %.1f ns/merge, %.0f merges/sec "
               "(%d iterations x %d merges)\n",
               nsPerMerge, mergesPerSec, mergeIterations, numHlls - 1);

        /* Cleanup */
        for (int i = 0; i < numHlls; i++) {
            hyperloglogFree(hlls[i]);
        }
    }

    return errors;
}
#endif

#if 0
/* PFDEBUG <subcommand> <key> ... args ...
 * Different debugging related operations about the HLL implementation. */
void pfdebugCommand(char *cmd, hyperloglogHeader *hdr) {
    robj *o;
    int32_t j;

    o = lookupKeyWrite(c->db, c->argv[2]);
    if (o == NULL) {
        addReplyError(c, "The specified key does not exist");
        return;
    }
    if (!hyperloglogDetect(c, o)) {
        return;
    }
    o = dbUnshareStringValue(c->db, c->argv[2], o);
    hdr = o->ptr;

    /* PFDEBUG GETREG <key> */
    if (!strcasecmp(cmd, "getreg")) {
        if (c->argc != 3) {
            goto arityerr;
        }

        if (isSparse(hdr)) {
            if (hyperloglogSparseToDense(o) == false) {
                addReplymdsc(c, mdscnew(invalidHllErr));
                return;
            }
            server.dirty++; /* Force propagation on encoding change. */
        }

        hdr = o->ptr;
        addReplyMultiBulkLen(c, HLL_REGISTERS);
        for (j = 0; j < HLL_REGISTERS; j++) {
            uint8_t val;

            HLL_DENSE_GET_REGISTER(val, hdr->registers, j);
            addReplyLongLong(c, val);
        }
    }
    /* PFDEBUG DECODE <key> */
    else if (!strcasecmp(cmd, "decode")) {
        if (c->argc != 3) {
            goto arityerr;
        }

        uint8_t *p = o->ptr, *end = p + mdsclen(o->ptr);
        mdsc *decoded = mdscempty();

        if (hdr->encoding != HLL_SPARSE) {
            addReplyError(c, "HLL encoding is not sparse");
            return;
        }

        p += HLL_HDR_SIZE;
        while (p < end) {
            int32_t runlen, regval;

            if (HLL_SPARSE_IS_ZERO(p)) {
                runlen = HLL_SPARSE_ZERO_LEN(p);
                p++;
                decoded = mdsccatprintf(decoded, "z:%d ", runlen);
            } else if (HLL_SPARSE_IS_XZERO(p)) {
                runlen = HLL_SPARSE_XZERO_LEN(p);
                p += 2;
                decoded = mdsccatprintf(decoded, "Z:%d ", runlen);
            } else {
                runlen = HLL_SPARSE_VAL_LEN(p);
                regval = HLL_SPARSE_VAL_VALUE(p);
                p++;
                decoded = mdsccatprintf(decoded, "v:%d,%d ", regval, runlen);
            }
        }
        decoded = mdsctrim(decoded, " ");
        addReplyBulkCBuffer(c, decoded, mdsclen(decoded));
        mdscfree(decoded);
    }
    /* PFDEBUG ENCODING <key> */
    else if (!strcasecmp(cmd, "encoding")) {
        char *encodingstr[2] = {"dense", "sparse"};
        if (c->argc != 3) {
            goto arityerr;
        }

        addReplyStatus(c, encodingstr[hdr->encoding]);
    }
    /* PFDEBUG TODENSE <key> */
    else if (!strcasecmp(cmd, "todense")) {
        int32_t conv = 0;
        if (c->argc != 3) {
            goto arityerr;
        }

        if (isSparse(hdr)) {
            if (hyperloglogSparseToDense(o) == false) {
                addReplymdsc(c, mdscnew(invalidHllErr));
                return;
            }
            conv = 1;
            server.dirty++; /* Force propagation on encoding change. */
        }
        addReply(c, conv ? shared.cone : shared.czero);
    } else {
        addReplyErrorFormat(c, "Unknown PFDEBUG subcommand '%s'", cmd);
    }
    return;

arityerr:
    addReplyErrorFormat(c, "Wrong number of arguments for the '%s' subcommand",
            cmd);
}
#endif

/* Originally:
 * hyperloglog.c - Redis HyperLogLog probabilistic cardinality approximation.
 * This file implements the algorithm and the exported Redis commands.
 *
 * Copyright (c) 2014, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
