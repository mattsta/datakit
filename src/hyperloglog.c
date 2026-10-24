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

#include "cleaner.h"
#include "datakit.h"
#include "mdsc.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h> /* va_arg */

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
/* xxhash's XXH_malloc/XXH_free wrappers call libc malloc/free, and the
 * header defines them unconditionally, so an inlined copy trips
 * datakit.h's deprecated redeclaration of both.  XXH_NO_STDLIB is
 * upstream's switch for an embedding with no library allocator; its
 * only cost is XXH*_createState(), which nothing here calls. */
#ifndef XXH_NO_STDLIB
#define XXH_NO_STDLIB
#endif
#if 0
#define XXH_FORCE_MEMORY_ACCESS 2
#endif
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

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(mdsc *, mdscfree);

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
        const uint8_t *_p = (const uint8_t *)p;                                \
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
        for (uint_fast32_t j = 0; j < 1024; j++) {
            uint64_t r0 = r[0] & 63;
            uint64_t r1 = (r[0] >> 6 | r[1] << 2) & 63;
            uint64_t r2 = (r[1] >> 4 | r[2] << 4) & 63;
            uint64_t r3 = (r[2] >> 2) & 63;
            uint64_t r4 = r[3] & 63;
            uint64_t r5 = (r[3] >> 6 | r[4] << 2) & 63;
            uint64_t r6 = (r[4] >> 4 | r[5] << 4) & 63;
            uint64_t r7 = (r[5] >> 2) & 63;
            uint64_t r8 = r[6] & 63;
            uint64_t r9 = (r[6] >> 6 | r[7] << 2) & 63;
            uint64_t r10 = (r[7] >> 4 | r[8] << 4) & 63;
            uint64_t r11 = (r[8] >> 2) & 63;
            uint64_t r12 = r[9] & 63;
            uint64_t r13 = (r[9] >> 6 | r[10] << 2) & 63;
            uint64_t r14 = (r[10] >> 4 | r[11] << 4) & 63;
            uint64_t r15 = (r[11] >> 2) & 63;

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
    cleaner(mdscfree) mdsc *dense = NULL;
    hyperloglogHeader *hdr;
    hyperloglogHeader *oldhdr = h;
    int32_t idx = 0;
    int32_t runlen;
    int32_t regval;
    uint8_t *p = (uint8_t *)sparse;
    const uint8_t *end = p + mdsclen(sparse);

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
        assert(NULL && "Conversion error?");
        return false;
    }

    /* Free original representation and set the new one. */
    hyperloglogFree(*inHll);
    *inHll = toHLL(dense);
    dense = NULL;

    return true;
}

/* "Add" the element in the sparse hyperloglog data structure.
 * Actually nothing is added, but the max 0 pattern counter of the subset
 * the element belongs to is incremented if needed.
 *
 * '*inHll' is the sketch; it is taken by reference because the update may
 * enlarge (reallocate) it.
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
     *    In this case the add returns 0 since no changes are performed.
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
     * convert from sparse to dense a register requires to be updated: the
     * add always reports a change when it converted the sketch. */
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
 * which is only used internally to count a union of several sketches. */
static void hyperloglogRawRegisterHistogram(uint8_t *registers,
                                            int32_t *reghisto) {
#if HLL_USE_SSE
    /* SSE2 optimized: check 16 bytes at a time for all-zeros */
    const __m128i *vec = (const __m128i *)registers;
    const __m128i zero = _mm_setzero_si128();

    for (int_fast32_t j = 0; j < HLL_REGISTERS / 16; j++) {
        __m128i chunk = _mm_loadu_si128(&vec[j]);
        __m128i cmp = _mm_cmpeq_epi8(chunk, zero);
        int mask = _mm_movemask_epi8(cmp);

        if (mask == 0xFFFF) {
            /* All 16 bytes are zero */
            reghisto[0] += 16;
        } else {
            /* At least one non-zero byte - process individually */
            const uint8_t *bytes = (const uint8_t *)&vec[j];
            for (int k = 0; k < 16; k++) {
                reghisto[bytes[k]]++;
            }
        }
    }

#elif HLL_USE_NEON
    /* ARM NEON optimized: check 16 bytes at a time for all-zeros */
    const uint8_t *data = registers;
    const uint8x16_t zero = vdupq_n_u8(0);

    for (int_fast32_t j = 0; j < HLL_REGISTERS / 16; j++, data += 16) {
        uint8x16_t chunk = vld1q_u8(data);
        uint8x16_t cmp = vceqq_u8(chunk, zero);

        /* Check if all bytes are zero using horizontal min.
         * vceqq_u8 returns 0xFF for matching bytes.
         * If min of comparison is 0xFF, all bytes matched zero */
        if (vminvq_u8(cmp) == 0xFF) {
            /* All 16 bytes are zero */
            reghisto[0] += 16;
        } else {
            /* At least one non-zero byte - process individually */
            for (int k = 0; k < 16; k++) {
                reghisto[data[k]]++;
            }
        }
    }

#else
    /* Scalar fallback: check 8 bytes at a time */
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
#endif
}

#ifdef DATAKIT_TEST
/* Scalar baseline for benchmarking comparison */
static void hyperloglogRawRegisterHistogramScalar(uint8_t *registers,
                                                  int32_t *reghisto) {
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
#endif

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
 * mean of the registers values. 'hdr' points to the sketch (its header
 * followed by its registers).
 *
 * If the sketch is not valid (a corrupt sparse representation, or an
 * encoding none of these), '*invalid' (when given) is set and the count is
 * 0; otherwise it is left untouched.
 *
 * Besides the dense and sparse encodings this reads the internal HLL_RAW
 * one, whose registers are a uint8_t array of HLL_REGISTERS entries: a
 * union of several sketches is counted from it without packing 6-bit
 * registers. */
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
        /* No representation this code reads: the sketch is invalid, and
         * has no cardinality to report */
        if (invalid) {
            *invalid = true;
        }
        return 0;
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

/* The cached cardinality when valid; else counted, and cached unless the
 * sketch is corrupt (0 and *invalid set then) */
uint64_t hyperloglogCount(hyperloglog *hdr, bool *invalid) {
    bool bad = false;
    if (HLL_VALID_CACHE(hdr)) {
        if (invalid) {
            *invalid = false;
        }
        return hdr->cardinality;
    }

    const uint64_t card = hyperloglogCount_(hdr, &bad);
    if (invalid) {
        *invalid = bad;
    }
    if (bad) {
        return 0;
    }

    if (card < HLL_CARDINALITY_MAX) {
        hdr->cardinality = card;
        hdr->cardinalityValid = true;
    }

    return card;
}

/* Call hyperloglogDenseAdd() or hyperloglogSparseAdd() according to the HLL
 * encoding.  An add that changes a register invalidates the cached
 * cardinality (the sparse path does so itself). */
int hyperloglogAdd(hyperloglog **inHll, const void *data, size_t size) {
    hyperloglogHeader *hdr = *inHll;
    if (hdr->encoding == HLL_DENSE) {
        const int changed = hyperloglogDenseAdd(hdr->registers, data, size);
        if (changed > 0) {
            HLL_INVALIDATE_CACHE(hdr);
        }
        return changed;
    }

    /* else, is sparse */
    return hyperloglogSparseAdd(inHll, data, size);
}

int64_t hyperloglogAddMany(hyperloglog **inHll, const void *const *data,
                           const size_t *lens, size_t n) {
    int64_t updated = 0;
    for (size_t i = 0; i < n; i++) {
        const int added = hyperloglogAdd(inHll, data[i], lens[i]);
        if (added < 0) {
            return -1;
        }
        updated += added;
    }
    return updated;
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

    /* @own: caller; free=hyperloglogFree */
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
    /* @own: caller; free=hyperloglogFree */
    return hyperloglogNew();
}

hyperloglog *hyperloglogNewDense(void) {
    /* @own: caller; free=hyperloglogFree */
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

    /* @own: caller; free=hyperloglogFree */
    return toHLL(mdscnewlen(toMDSC(src), mdsclen(toMDSC(src))));
}

void hyperloglogInvalidateCache(hyperloglog *h) {
    HLL_INVALIDATE_CACHE(h);
}

/* Check if the object is a String with a valid HLL representation.
 * Return true if this is true, otherwise reply to the client
 * with an error and return false. */
bool hyperloglogDetect(hyperloglog *h) {
    if (!h) {
        return false;
    }

    const size_t len = mdsclen(toMDSC(h));

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

/* Fold every sketch of 'hlls' into the RAW scratch 'raw' (register-wise
 * max), noting whether any is dense; false if one is corrupt */
static bool hyperloglogFoldAll_(hyperloglog *raw,
                                const hyperloglog *const *hlls, size_t n,
                                bool *anyDense) {
    for (size_t i = 0; i < n; i++) {
        if (!hyperloglogMerge(raw, hlls[i])) {
            return false;
        }
        if (hlls[i]->encoding == HLL_DENSE) {
            *anyDense = true;
        }
    }
    return true;
}

uint64_t hyperloglogCountUnion(const hyperloglog *const *hlls, size_t n,
                               bool *invalid) {
    hllStatic max = {0};
    hyperloglog *raw = toHLL(max);
    raw->encoding = HLL_RAW; /* Special internal-only encoding. */

    bool anyDense = false;
    const bool ok = hyperloglogFoldAll_(raw, hlls, n, &anyDense);
    if (invalid) {
        *invalid = !ok;
    }
    return ok ? hyperloglogCount_(raw, NULL) : 0;
}

hyperloglog *hyperloglogNewFromRaw(const hyperloglog *raw, bool dense) {
    hyperloglog *result =
        dense ? hyperloglogNewDense() : hyperloglogNewSparse();
    /* Read through raw->registers — the RAW register array starts AFTER
     * the header at the front of the scratch buffer; indexing the buffer
     * directly would read header bytes as registers 0..7. */
    const uint8_t *const rawRegs = raw->registers;
    for (size_t i = 0; i < HLL_REGISTERS; i++) {
        if (rawRegs[i] == 0) {
            continue;
        }

        /* Re-check encoding every iteration: a sparse set can promote
         * to dense mid-loop when the sparse form outgrows its bound. */
        if (result->encoding == HLL_DENSE) {
            hyperloglogDenseSet(result->registers, i, rawRegs[i]);
        } else {
            hyperloglogSparseSet(&result, i, rawRegs[i]);
        }
    }

    HLL_INVALIDATE_CACHE(result);
    return result;
}

bool hyperloglogMergeInto(hyperloglog **dst, const hyperloglog *src) {
    assert(src->encoding != HLL_RAW);

    /* Accumulate max(dst, src) into a RAW scratch, then write the
     * non-zero registers back through the max-semantics setters (they
     * only raise, never lower, so the write-back composes with any
     * registers dst gained in between — the fold is a true max). */
    hllStatic scratchBuf = {0};
    hyperloglog *scratch = toHLL(scratchBuf);
    scratch->encoding = HLL_RAW;

    if (!hyperloglogMerge(scratch, *dst)) {
        return false;
    }

    if (!hyperloglogMerge(scratch, src)) {
        return false;
    }

    if (src->encoding == HLL_DENSE && (*dst)->encoding == HLL_SPARSE &&
        !hyperloglogSparseToDense(dst)) {
        return false;
    }

    const uint8_t *const rawRegs = scratch->registers;
    for (size_t i = 0; i < HLL_REGISTERS; i++) {
        if (rawRegs[i] == 0) {
            continue;
        }

        if ((*dst)->encoding == HLL_DENSE) {
            hyperloglogDenseSet((*dst)->registers, i, rawRegs[i]);
        } else {
            hyperloglogSparseSet(dst, i, rawRegs[i]);
        }
    }

    HLL_INVALIDATE_CACHE(*dst);
    return true;
}

hyperloglog *hyperloglogNewUnion(const hyperloglog *const *hlls, size_t n) {
    hllStatic max = {0};
    hyperloglog *raw = toHLL(max);
    raw->encoding = HLL_RAW; /* Special internal-only encoding. */

    /* Dense when any input is (a union is at least as full as each) */
    bool anyDense = false;
    if (!hyperloglogFoldAll_(raw, hlls, n, &anyDense)) {
        return NULL;
    }

    /* @own: caller; free=hyperloglogFree */
    return hyperloglogNewFromRaw(raw, anyDense);
}

/* ========================== Testing / Debugging  ========================== */

#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"
#include "timeUtil.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint8_t *, zfree, U8);
defcleaner(hyperloglog *, hyperloglogFree);

/* Register-access cycles; the approximation test adds 10000x as many
 * elements (10M), still crossing every sparse/dense checkpoint */
#define HLL_TEST_CYCLES 1000
int hyperloglogTest(int argc, char *argv[]) {
    DK_NOTUSED(argc);
    DK_NOTUSED(argv);

    uint32_t sseed = time(NULL) ^ getpid();
    uint32_t shash = HYPERHASH(&sseed, sizeof(sseed), 0xadc83b19ULL);
    srand(shash);

    printf("Testing (using seed: %" PRIu32 ") for %" PRId32 " cycles\n",
           (uint32_t)shash, (int32_t)HLL_TEST_CYCLES);

    cleaner(mdscfree) mdsc *bitcounters = mdscnewlen(NULL, HLL_DENSE_SIZE);
    hyperloglogHeader *hdr = (hyperloglogHeader *)bitcounters, *hdr2;
    uint8_t bytecounters[HLL_REGISTERS] = {0};
    int err = 0;
    cleaner(hyperloglogFree) hyperloglog *h = NULL;

    /* Every register is accessible, and setting one keeps its value without
     * touching its neighbours */
    TEST("register access: each register holds its value") {
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
                    ERR("register %" PRIu32 " should be %" PRIu8
                        " but is %" PRIu32,
                        (uint32_t)i, (uint8_t)bytecounters[i], (uint32_t)val);
                    goto approximationDone;
                }
            }
        }
    }

    /* Unique elements keep the estimate within a few standard errors (so a
     * "bad" run is very unlikely to fail it), with a dense and a sparse
     * sketch fed together agreeing on every checkpoint's count */
    TEST("approximation error: dense and sparse agree within bounds") {
        memset(hdr->registers, 0, HLL_DENSE_SIZE - HLL_HDR_SIZE);
        h = hyperloglogNew();
        double relerr = 1.04 / sqrt(HLL_REGISTERS);
        uint64_t checkpoint = 1;
        uint64_t seed = (uint64_t)rand() | (uint64_t)rand() << 32;
        uint64_t ele;
        /* Checkpoints 1, 10, ... 10^6: past the sparse-to-dense switch and the
         * linear-counting range (about 2.5 x HLL_REGISTERS), well into the
         * raw-estimate range */
        uint64_t testCount = HLL_TEST_CYCLES * 1000;
        for (uint64_t j = 1; j <= testCount; j++) {
            if (j % 500000 == 0) {
                printf("[approximation error]: Testing cycle %" PRIu64
                       " (%0.2f%% done)\n",
                       j, (float)j / testCount * 100);
            }
            ele = j ^ seed;
            /* Through hyperloglogAdd, as a caller would: it invalidates the
             * cached count when a register changes (a dense add never moves
             * the buffer) */
            hyperloglogAdd(&hdr, &ele, sizeof(ele));
            hyperloglogAdd(&h, &ele, sizeof(ele));

            /* Make sure for small cardinalities we use sparse encoding. */
            if (j == checkpoint && j < serverSparseMaxBytes / 2) {
                hdr2 = h;
                if (hdr2->encoding != HLL_SPARSE) {
                    ERR("sparse encoding not used at %" PRIu64, j);
                    goto approximationDone;
                }
            }

            /* Check that dense and sparse representations agree. */
            if (j == checkpoint &&
                hyperloglogCount(hdr, NULL) != hyperloglogCount(h, NULL)) {
                ERR("dense/sparse disagree at %" PRIu64, j);
                goto approximationDone;
            }

            /* Check error. */
            if (j == checkpoint) {
                int64_t abserr =
                    checkpoint - (int64_t)hyperloglogCount(hdr, NULL);
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
                    ERR("error too big: card %" PRIu64 ", abserr %" PRId64,
                        checkpoint, abserr);
                    goto approximationDone;
                }
                checkpoint *= 10;
            }
        }
    }

approximationDone:
    /* Merging dense sketches of disjoint elements counts about their sum */
    TEST("merge correctness: a dense union counts both inputs") {
        hyperloglog *h1 = hyperloglogNewDense();
        hyperloglog *h2 = hyperloglogNewDense();

        /* Add different elements to each HLL */
        for (uint64_t i = 0; i < 10000; i++) {
            uint64_t v1 = i * 2;
            uint64_t v2 = i * 2 + 1;
            hyperloglogAdd(&h1, &v1, sizeof(v1));
            hyperloglogAdd(&h2, &v2, sizeof(v2));
        }

        uint64_t count1 = hyperloglogCount(h1, NULL);
        uint64_t count2 = hyperloglogCount(h2, NULL);

        /* Merge h2 into a new HLL that also has h1's data */
        const hyperloglog *pair[2] = {h1, h2};
        hyperloglog *merged = hyperloglogNewUnion(pair, 2);
        uint64_t countMerged = hyperloglogCount(merged, NULL);

        /* Merged count should be approximately count1 + count2 */
        /* Allow 10% error margin */
        uint64_t expectedMin = (count1 + count2) * 90 / 100;
        uint64_t expectedMax = (count1 + count2) * 110 / 100;

        if (countMerged < expectedMin || countMerged > expectedMax) {
            ERR("merge count %" PRIu64 " not in [%" PRIu64 ", %" PRIu64
                "] (h1=%" PRIu64 ", h2=%" PRIu64 ")",
                countMerged, expectedMin, expectedMax, count1, count2);
        }

        hyperloglogFree(h1);
        hyperloglogFree(h2);
        hyperloglogFree(merged);
    }

    /* Merge throughput (informational) */
    TEST("merge benchmark: chained dense unions") {
        /* Smoke size: the timing printed is informational (measure merges
         * under the perf preset, not in this correctness suite) */
        const int numHlls = 100;
        const int mergeIterations = 10;

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
            const hyperloglog *first[2] = {hlls[0], hlls[1]};
            hyperloglog *result = hyperloglogNewUnion(first, 2);
            for (int i = 2; i < numHlls; i++) {
                const hyperloglog *next[2] = {result, hlls[i]};
                hyperloglog *newResult = hyperloglogNewUnion(next, 2);
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

    /* The SIMD register histogram matches the scalar one; timings are
     * informational */
    TEST("register histogram: SIMD matches scalar") {
        /* Smoke size, as for the merge benchmark above */
        const int histoIterations = 100;

        /* Create test registers: mix of zeros and values */
        cleanerName(zfree, U8) uint8_t *testRegisters =
            zcalloc(HLL_REGISTERS, sizeof(uint8_t));

        /* Seed with some non-zero values (simulating a partially filled HLL)
         * Use values 1 to HLL_Q (50) which are valid for the histogram array */
        for (int i = 0; i < HLL_REGISTERS / 4; i++) {
            testRegisters[i * 4] = (i % HLL_Q) + 1;
        }

        int32_t reghistoSIMD[HLL_Q + 2] = {0};
        int32_t reghistoScalar[HLL_Q + 2] = {0};

        /* Benchmark SIMD version */
        uint64_t startSIMD = timeUtilMonotonicNs();
        for (int iter = 0; iter < histoIterations; iter++) {
            memset(reghistoSIMD, 0, sizeof(reghistoSIMD));
            hyperloglogRawRegisterHistogram(testRegisters, reghistoSIMD);
        }
        uint64_t endSIMD = timeUtilMonotonicNs();

        /* Benchmark Scalar version */
        uint64_t startScalar = timeUtilMonotonicNs();
        for (int iter = 0; iter < histoIterations; iter++) {
            memset(reghistoScalar, 0, sizeof(reghistoScalar));
            hyperloglogRawRegisterHistogramScalar(testRegisters,
                                                  reghistoScalar);
        }
        uint64_t endScalar = timeUtilMonotonicNs();

        /* Verify correctness: SIMD result should match scalar */
        memset(reghistoSIMD, 0, sizeof(reghistoSIMD));
        memset(reghistoScalar, 0, sizeof(reghistoScalar));
        hyperloglogRawRegisterHistogram(testRegisters, reghistoSIMD);
        hyperloglogRawRegisterHistogramScalar(testRegisters, reghistoScalar);

        int histoMismatch = 0;
        for (int i = 0; i < HLL_Q + 2; i++) {
            if (reghistoSIMD[i] != reghistoScalar[i]) {
                if (histoMismatch < 5) {
                    printf("  histogram mismatch at [%d]: SIMD=%d Scalar=%d\n",
                           i, reghistoSIMD[i], reghistoScalar[i]);
                }
                histoMismatch++;
            }
        }
        if (histoMismatch) {
            ERR("histogram: %d SIMD/scalar mismatches", histoMismatch);
        }

        double nsPerHistoSIMD = (double)(endSIMD - startSIMD) / histoIterations;
        double nsPerHistoScalar =
            (double)(endScalar - startScalar) / histoIterations;
        double speedup = nsPerHistoScalar / nsPerHistoSIMD;

        printf("  SIMD: %.1f ns/histogram, Scalar: %.1f ns/histogram\n",
               nsPerHistoSIMD, nsPerHistoScalar);
        printf("  Speedup: %.2fx\n", speedup);
    }

    TEST("the estimator's tau matches its series") {
        /* tau(x) = (1 - x - sum_k (1 - x^(2^-k))^2 2^-k) / 3, summed here
         * with pow over 60 terms (past double precision): the estimator's
         * iterative form agrees to 1e-12, and is 0 at both ends */
        static const double xs[] = {1e-6, 0.001, 0.25, 0.5, 0.75, 0.999};
        bool agree = hllTau(0.0) == 0.0 && hllTau(1.0) == 0.0;
        for (size_t i = 0; i < sizeof(xs) / sizeof(*xs); i++) {
            double sum = 0.0;
            for (int k = 1; k <= 60; k++) {
                const double root = pow(xs[i], ldexp(1.0, -k));
                sum += (1.0 - root) * (1.0 - root) * ldexp(1.0, -k);
            }
            const double want = (1.0 - xs[i] - sum) / 3.0;
            agree = agree && fabs(hllTau(xs[i]) - want) < 1e-12;
        }
        if (!agree) {
            ERRR("tau differs from its series");
        }
    }

    TEST("a sketch with no known encoding counts 0, reported invalid") {
        /* An encoding byte that names no representation: the count is 0
         * and the sketch reported invalid, not a count of UINT64_MAX */
        hyperloglog *unk = hyperloglogNewDense();
        const uint8_t saved = unk->encoding;
        unk->encoding = 3; /* the 2-bit field's one unused value */
        bool invalid = false;
        const uint64_t count = hyperloglogCount(unk, &invalid);
        /* A union over a corrupt sketch: no count, no sketch, and a
         * corrupt count is never cached */
        hyperloglog *fine = hyperloglogNewDense();
        const hyperloglog *withBad[2] = {fine, unk};
        bool unionBad = false;
        const uint64_t unionOf = hyperloglogCountUnion(withBad, 2, &unionBad);
        hyperloglog *unionSketch = hyperloglogNewUnion(withBad, 2);
        bool again = false;
        hyperloglogCount(unk, &again);
        unk->encoding = saved;
        hyperloglogFree(fine);
        if (unionOf != 0 || !unionBad || unionSketch || !again) {
            ERR("corrupt sketch: union %" PRIu64
                " (invalid %d), sketch %p, recount invalid %d",
                unionOf, unionBad, (void *)unionSketch, again);
            hyperloglogFree(unionSketch);
        }
        if (count != 0 || !invalid) {
            ERR("unknown encoding: count %" PRIu64 ", invalid %d", count,
                invalid);
        }
        hyperloglogFree(unk);
    }

    TEST("a dense merge into a sparse sketch holds the register max") {
        /* A sparse sketch of 10 elements takes a dense one of 5000: it
         * turns dense and holds the register-wise maximum, so its count
         * is exactly that of the dense sketch with the 10 elements
         * added */
        hyperloglog *few = hyperloglogNewSparse();
        hyperloglog *many = hyperloglogNewDense();
        /* hashed by strlen: the bytes in the buffer, never snprintf's
         * would-have-written length */
        char elem[24];
        for (int i = 0; i < 10; i++) {
            snprintf(elem, sizeof(elem), "few-%d", i);
            hyperloglogAdd(&few, elem, strlen(elem));
        }
        for (int i = 0; i < 5000; i++) {
            snprintf(elem, sizeof(elem), "many-%d", i);
            hyperloglogAdd(&many, elem, strlen(elem));
        }
        hyperloglog *oracle = hyperloglogCopy(many);
        for (int i = 0; i < 10; i++) {
            snprintf(elem, sizeof(elem), "few-%d", i);
            hyperloglogAdd(&oracle, elem, strlen(elem));
        }
        const bool wasSparse = isSparse(few);
        const bool merged = hyperloglogMergeInto(&few, many);
        bool invalid = false;
        const uint64_t got = hyperloglogCount(few, &invalid);
        const uint64_t want = hyperloglogCount(oracle, &invalid);
        if (!wasSparse || !merged || !isDense(few) || invalid || got != want) {
            ERR("dense into sparse: merged %d, dense %d, count %" PRIu64
                " want %" PRIu64,
                merged, isDense(few), got, want);
        }
        hyperloglogFree(few);
        hyperloglogFree(many);
        hyperloglogFree(oracle);
    }

    TEST("batch adds report the changed registers, both encodings") {
        /* One call adds every element and reports how many changed a
         * register: 3 new, then a repeat and a fresh one (1), then none */
        for (int dense = 0; dense <= 1; dense++) {
            cleaner(hyperloglogFree) hyperloglog *sk =
                dense ? hyperloglogNewDense() : hyperloglogNewSparse();
            const void *const first[3] = {"alpha", "beta", "gamma"};
            static const size_t firstLens[3] = {5, 4, 5};
            const void *const second[2] = {"alpha", "delta"};
            static const size_t secondLens[2] = {5, 5};
            const int64_t a = hyperloglogAddMany(&sk, first, firstLens, 3);
            const int64_t b = hyperloglogAddMany(&sk, second, secondLens, 2);
            const int64_t c = hyperloglogAddMany(&sk, second, secondLens, 0);
            bool invalid = false;
            const uint64_t count = hyperloglogCount(sk, &invalid);
            if (a != 3 || b != 1 || c != 0 || count != 4 || invalid) {
                ERR("%s batch: %" PRId64 ", %" PRId64 ", %" PRId64
                    ", count %" PRIu64,
                    dense ? "dense" : "sparse", a, b, c, count);
            }
        }
    }

    TEST("the cached count follows adds, both encodings") {
        /* Counting caches the estimate; an add that changes a register
         * invalidates it (the dense path used not to, so a dense sketch
         * kept serving its old count), a repeat add changes nothing */
        for (int dense = 0; dense <= 1; dense++) {
            cleaner(hyperloglogFree) hyperloglog *sk =
                dense ? hyperloglogNewDense() : hyperloglogNewSparse();
            static const char *const words[] = {"alpha", "beta", "gamma"};
            for (size_t i = 0; i < 3; i++) {
                hyperloglogAdd(&sk, words[i], strlen(words[i]));
            }
            bool invalid = false;
            const uint64_t three = hyperloglogCount(sk, &invalid);
            const int repeat = hyperloglogAdd(&sk, "alpha", 5);
            const uint64_t stillThree = hyperloglogCount(sk, &invalid);
            const int fresh = hyperloglogAdd(&sk, "delta", 5);
            const uint64_t four = hyperloglogCount(sk, &invalid);
            if (three != 3 || repeat != 0 || stillThree != 3 || fresh != 1 ||
                four != 4 || invalid) {
                ERR("%s cache: %" PRIu64 ", repeat %d, %" PRIu64
                    ", fresh %d, %" PRIu64,
                    dense ? "dense" : "sparse", three, repeat, stillThree,
                    fresh, four);
            }
        }
    }

    TEST("a copy is independent; union counts agree with the merge") {
        /* A copy is independent of its source; the union count of several
         * sketches equals the count of their merge and of the new union
         * sketch; no sketches count 0 and union to an empty sketch */
        cleaner(hyperloglogFree) hyperloglog *a = hyperloglogNewSparse();
        cleaner(hyperloglogFree) hyperloglog *b = hyperloglogNew();
        bool invalid = false;
        if (hyperloglogCount(a, &invalid) != 0 || invalid) {
            ERRR("a new sparse HLL is not empty");
        }
        /* Fixed-size keys: members 0..1499, a holds 0..999, b 500..1499 */
        for (uint64_t i = 0; i < 1500; i++) {
            if (i < 1000) {
                hyperloglogAdd(&a, &i, sizeof(i));
            }
            if (i >= 500) {
                hyperloglogAdd(&b, &i, sizeof(i));
            }
        }
        hyperloglogInvalidateCache(a);
        hyperloglogInvalidateCache(b);

        const uint64_t countA = hyperloglogCount(a, &invalid);
        cleaner(hyperloglogFree) hyperloglog *c = hyperloglogCopy(a);
        for (uint64_t i = 100000; i < 100200; i++) {
            hyperloglogAdd(&c, &i, sizeof(i));
        }
        hyperloglogInvalidateCache(a);

        cleaner(hyperloglogFree) hyperloglog *merged = hyperloglogCopy(a);
        hyperloglogMergeInto(&merged, b);
        const hyperloglog *ab[2] = {a, b};
        const uint64_t unionCount = hyperloglogCountUnion(ab, 2, &invalid);
        const uint64_t mergedCount = hyperloglogCount(merged, &invalid);
        cleaner(hyperloglogFree) hyperloglog *u = hyperloglogNewUnion(ab, 2);
        cleaner(hyperloglogFree) hyperloglog *none = hyperloglogNewUnion(ab, 0);
        if (hyperloglogCount(a, &invalid) != countA ||
            unionCount != mergedCount || unionCount < 1400 ||
            unionCount > 1600 || !u ||
            hyperloglogCount(u, &invalid) != unionCount || !none ||
            hyperloglogCount(none, &invalid) != 0 ||
            hyperloglogCountUnion(ab, 0, &invalid) != 0 || invalid) {
            ERR("copy/union: a %" PRIu64 " (was %" PRIu64 "), union %" PRIu64
                ", merged %" PRIu64,
                hyperloglogCount(a, &invalid), countA, unionCount, mergedCount);
        }
    }

    TEST_FINAL_RESULT;
}
#endif

/* Originally:
 * hyperloglog.c - HyperLogLog probabilistic cardinality approximation.
 * This file implements the algorithm and the exported commands.
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
