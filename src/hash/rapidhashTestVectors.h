#pragma once

/* rapidhash V3 test vectors - captured from the upstream reference
 * implementation by Nicolas De Carli (BSD-style license,
 * https://github.com/Nicoshev/rapidhash). Used to KAT-validate the
 * datakit port bit-for-bit.
 *
 * All three exported variants are covered at every (length, seed):
 *   - rapidhash_withSeed      (main / fastest on large inputs)
 *   - rapidhashMicro_withSeed (smaller code, narrower unroll)
 *   - rapidhashNano_withSeed  (nano variant, lowest throughput at scale)
 *
 * The default secret table (rapid_secret[]) was used for capture.
 *
 * Input bytes are NOT stored here; the test driver regenerates them at
 * runtime using a deterministic fill rule so the vectors remain
 * reproducible from this header alone:
 *
 *     buf[i] = (uint8_t)(i * 131u + 17u);   for i in [0, length)
 *
 * Tag this rule "deterministic-fill-v1" when re-deriving inputs.
 *
 * Length coverage exercises every code path in the reference:
 *   - 0                            (empty / fast-path return)
 *   - 1, 2, 3                      (<4-byte mix: p[0]<<45 | p[len-1])
 *   - 4, 5, 7                      (4..7 32-bit pair branch)
 *   - 8, 9, 15, 16                 (8..16 64-bit pair branch)
 *   - 17, 32, 48, 49               (Nano: past bulk threshold;
 *                                   Micro/Main: 17..80 tail)
 *   - 64, 79, 80, 81               (Nano: past bulk;
 *                                   Micro: just past its threshold)
 *   - 96, 111, 112, 113            (Main: just past its threshold)
 *   - 200, 224, 225                (Main: past unroll boundary)
 *   - 1024                         (medium bulk)
 *   - 8192, 65536                  (large bulk)
 *
 * Seeds: 0, 0xdeadbeefcafebabe, 0x0123456789abcdef.
 */

#include <stddef.h>
#include <stdint.h>

typedef struct rapidhashTestVector {
    const char *name;
    size_t input_len;
    uint64_t seed;
    uint64_t expected_rapidhash;
    uint64_t expected_micro;
    uint64_t expected_nano;
} rapidhashTestVector;

/* clang-format off */
static const rapidhashTestVector rapidhashTestVectorsTable[] = {
    { "len=0     seed=0x0000000000000000", 0, 0x0000000000000000ULL, 0x0338dc4be2cecdaeULL, 0x0338dc4be2cecdaeULL, 0x0338dc4be2cecdaeULL },
    { "len=0     seed=0xdeadbeefcafebabe", 0, 0xdeadbeefcafebabeULL, 0x06371798fffbc11fULL, 0x06371798fffbc11fULL, 0x06371798fffbc11fULL },
    { "len=0     seed=0x0123456789abcdef", 0, 0x0123456789abcdefULL, 0x565ef32cd8efb3ddULL, 0x565ef32cd8efb3ddULL, 0x565ef32cd8efb3ddULL },
    { "len=1     seed=0x0000000000000000", 1, 0x0000000000000000ULL, 0x46911692db77b8aaULL, 0x46911692db77b8aaULL, 0x46911692db77b8aaULL },
    { "len=1     seed=0xdeadbeefcafebabe", 1, 0xdeadbeefcafebabeULL, 0x89beec39570b1923ULL, 0x89beec39570b1923ULL, 0x89beec39570b1923ULL },
    { "len=1     seed=0x0123456789abcdef", 1, 0x0123456789abcdefULL, 0xc573fbeb3fba0e83ULL, 0xc573fbeb3fba0e83ULL, 0xc573fbeb3fba0e83ULL },
    { "len=2     seed=0x0000000000000000", 2, 0x0000000000000000ULL, 0x2fe388953a79680eULL, 0x2fe388953a79680eULL, 0x2fe388953a79680eULL },
    { "len=2     seed=0xdeadbeefcafebabe", 2, 0xdeadbeefcafebabeULL, 0x2e29f450954c5f50ULL, 0x2e29f450954c5f50ULL, 0x2e29f450954c5f50ULL },
    { "len=2     seed=0x0123456789abcdef", 2, 0x0123456789abcdefULL, 0x3222f3fd9dfe0648ULL, 0x3222f3fd9dfe0648ULL, 0x3222f3fd9dfe0648ULL },
    { "len=3     seed=0x0000000000000000", 3, 0x0000000000000000ULL, 0x4681631b0268bdaeULL, 0x4681631b0268bdaeULL, 0x4681631b0268bdaeULL },
    { "len=3     seed=0xdeadbeefcafebabe", 3, 0xdeadbeefcafebabeULL, 0x88c395c0afa59a85ULL, 0x88c395c0afa59a85ULL, 0x88c395c0afa59a85ULL },
    { "len=3     seed=0x0123456789abcdef", 3, 0x0123456789abcdefULL, 0xdb0d3c076ee8cb1bULL, 0xdb0d3c076ee8cb1bULL, 0xdb0d3c076ee8cb1bULL },
    { "len=4     seed=0x0000000000000000", 4, 0x0000000000000000ULL, 0xffd257fe10b451ddULL, 0xffd257fe10b451ddULL, 0xffd257fe10b451ddULL },
    { "len=4     seed=0xdeadbeefcafebabe", 4, 0xdeadbeefcafebabeULL, 0x8e5205e4d9561344ULL, 0x8e5205e4d9561344ULL, 0x8e5205e4d9561344ULL },
    { "len=4     seed=0x0123456789abcdef", 4, 0x0123456789abcdefULL, 0x0d1f768e34415b45ULL, 0x0d1f768e34415b45ULL, 0x0d1f768e34415b45ULL },
    { "len=5     seed=0x0000000000000000", 5, 0x0000000000000000ULL, 0x51c72df2e6156c92ULL, 0x51c72df2e6156c92ULL, 0x51c72df2e6156c92ULL },
    { "len=5     seed=0xdeadbeefcafebabe", 5, 0xdeadbeefcafebabeULL, 0x8222604598f1ec51ULL, 0x8222604598f1ec51ULL, 0x8222604598f1ec51ULL },
    { "len=5     seed=0x0123456789abcdef", 5, 0x0123456789abcdefULL, 0x4c4a2940556e54e1ULL, 0x4c4a2940556e54e1ULL, 0x4c4a2940556e54e1ULL },
    { "len=7     seed=0x0000000000000000", 7, 0x0000000000000000ULL, 0x9aea172b6dd14b40ULL, 0x9aea172b6dd14b40ULL, 0x9aea172b6dd14b40ULL },
    { "len=7     seed=0xdeadbeefcafebabe", 7, 0xdeadbeefcafebabeULL, 0x6fb1f9694eb37a30ULL, 0x6fb1f9694eb37a30ULL, 0x6fb1f9694eb37a30ULL },
    { "len=7     seed=0x0123456789abcdef", 7, 0x0123456789abcdefULL, 0xe4f6e919677017acULL, 0xe4f6e919677017acULL, 0xe4f6e919677017acULL },
    { "len=8     seed=0x0000000000000000", 8, 0x0000000000000000ULL, 0xcb63385f26fb064eULL, 0xcb63385f26fb064eULL, 0xcb63385f26fb064eULL },
    { "len=8     seed=0xdeadbeefcafebabe", 8, 0xdeadbeefcafebabeULL, 0x2cadb9d9a7b4b02aULL, 0x2cadb9d9a7b4b02aULL, 0x2cadb9d9a7b4b02aULL },
    { "len=8     seed=0x0123456789abcdef", 8, 0x0123456789abcdefULL, 0x11ceea2609447849ULL, 0x11ceea2609447849ULL, 0x11ceea2609447849ULL },
    { "len=9     seed=0x0000000000000000", 9, 0x0000000000000000ULL, 0x69727c5eb48b255bULL, 0x69727c5eb48b255bULL, 0x69727c5eb48b255bULL },
    { "len=9     seed=0xdeadbeefcafebabe", 9, 0xdeadbeefcafebabeULL, 0x93c5b0e408d0864aULL, 0x93c5b0e408d0864aULL, 0x93c5b0e408d0864aULL },
    { "len=9     seed=0x0123456789abcdef", 9, 0x0123456789abcdefULL, 0x3521ae222648cd8fULL, 0x3521ae222648cd8fULL, 0x3521ae222648cd8fULL },
    { "len=15    seed=0x0000000000000000", 15, 0x0000000000000000ULL, 0x04448b895e0f96afULL, 0x04448b895e0f96afULL, 0x04448b895e0f96afULL },
    { "len=15    seed=0xdeadbeefcafebabe", 15, 0xdeadbeefcafebabeULL, 0xd6b568790b8a88a3ULL, 0xd6b568790b8a88a3ULL, 0xd6b568790b8a88a3ULL },
    { "len=15    seed=0x0123456789abcdef", 15, 0x0123456789abcdefULL, 0x30affe007caf01dfULL, 0x30affe007caf01dfULL, 0x30affe007caf01dfULL },
    { "len=16    seed=0x0000000000000000", 16, 0x0000000000000000ULL, 0xddd5b9d42bf20f35ULL, 0xddd5b9d42bf20f35ULL, 0xddd5b9d42bf20f35ULL },
    { "len=16    seed=0xdeadbeefcafebabe", 16, 0xdeadbeefcafebabeULL, 0x0306ce8a88ae6ce9ULL, 0x0306ce8a88ae6ce9ULL, 0x0306ce8a88ae6ce9ULL },
    { "len=16    seed=0x0123456789abcdef", 16, 0x0123456789abcdefULL, 0xad0695c771a3b561ULL, 0xad0695c771a3b561ULL, 0xad0695c771a3b561ULL },
    { "len=17    seed=0x0000000000000000", 17, 0x0000000000000000ULL, 0x4d3c0b9b9c4ed98eULL, 0x4d3c0b9b9c4ed98eULL, 0x4d3c0b9b9c4ed98eULL },
    { "len=17    seed=0xdeadbeefcafebabe", 17, 0xdeadbeefcafebabeULL, 0x8cf305389fcc0293ULL, 0x8cf305389fcc0293ULL, 0x8cf305389fcc0293ULL },
    { "len=17    seed=0x0123456789abcdef", 17, 0x0123456789abcdefULL, 0x4343af322371f509ULL, 0x4343af322371f509ULL, 0x4343af322371f509ULL },
    { "len=32    seed=0x0000000000000000", 32, 0x0000000000000000ULL, 0x7b1a19ca6d34cd0dULL, 0x7b1a19ca6d34cd0dULL, 0x7b1a19ca6d34cd0dULL },
    { "len=32    seed=0xdeadbeefcafebabe", 32, 0xdeadbeefcafebabeULL, 0x75f81084a6c8dba7ULL, 0x75f81084a6c8dba7ULL, 0x75f81084a6c8dba7ULL },
    { "len=32    seed=0x0123456789abcdef", 32, 0x0123456789abcdefULL, 0x1b3f879d9c1e8bacULL, 0x1b3f879d9c1e8bacULL, 0x1b3f879d9c1e8bacULL },
    { "len=48    seed=0x0000000000000000", 48, 0x0000000000000000ULL, 0xf07d49050a3c34eeULL, 0xf07d49050a3c34eeULL, 0xf07d49050a3c34eeULL },
    { "len=48    seed=0xdeadbeefcafebabe", 48, 0xdeadbeefcafebabeULL, 0x66d135935be91b17ULL, 0x66d135935be91b17ULL, 0x66d135935be91b17ULL },
    { "len=48    seed=0x0123456789abcdef", 48, 0x0123456789abcdefULL, 0x4e53824ba53f265cULL, 0x4e53824ba53f265cULL, 0x4e53824ba53f265cULL },
    { "len=49    seed=0x0000000000000000", 49, 0x0000000000000000ULL, 0x66f9fa0959d4c84cULL, 0x66f9fa0959d4c84cULL, 0xa4899997e3d92539ULL },
    { "len=49    seed=0xdeadbeefcafebabe", 49, 0xdeadbeefcafebabeULL, 0x82305ceab3373e8eULL, 0x82305ceab3373e8eULL, 0x93071ea71cf60983ULL },
    { "len=49    seed=0x0123456789abcdef", 49, 0x0123456789abcdefULL, 0x1baca26f512fb009ULL, 0x1baca26f512fb009ULL, 0xd9587d65d528404fULL },
    { "len=64    seed=0x0000000000000000", 64, 0x0000000000000000ULL, 0x54509a0ff905068aULL, 0x54509a0ff905068aULL, 0xe876a73ae2e97b2fULL },
    { "len=64    seed=0xdeadbeefcafebabe", 64, 0xdeadbeefcafebabeULL, 0xb39f33b1cf7664e5ULL, 0xb39f33b1cf7664e5ULL, 0xe77b42324c6726f5ULL },
    { "len=64    seed=0x0123456789abcdef", 64, 0x0123456789abcdefULL, 0x4940e6ba3b4c0628ULL, 0x4940e6ba3b4c0628ULL, 0x21dea1803f14a82bULL },
    { "len=79    seed=0x0000000000000000", 79, 0x0000000000000000ULL, 0x9269e0d371bf1ebdULL, 0x9269e0d371bf1ebdULL, 0x81010dd060a999ccULL },
    { "len=79    seed=0xdeadbeefcafebabe", 79, 0xdeadbeefcafebabeULL, 0x57732ac38051e9d6ULL, 0x57732ac38051e9d6ULL, 0x7bffa79f76425a66ULL },
    { "len=79    seed=0x0123456789abcdef", 79, 0x0123456789abcdefULL, 0x66c01a1e2f57dda4ULL, 0x66c01a1e2f57dda4ULL, 0xe23059cb88927769ULL },
    { "len=80    seed=0x0000000000000000", 80, 0x0000000000000000ULL, 0x048e4a0936098b98ULL, 0x048e4a0936098b98ULL, 0xfac79f0dd7189225ULL },
    { "len=80    seed=0xdeadbeefcafebabe", 80, 0xdeadbeefcafebabeULL, 0x3cb92ae575a1c90cULL, 0x3cb92ae575a1c90cULL, 0x20e3444507ca25caULL },
    { "len=80    seed=0x0123456789abcdef", 80, 0x0123456789abcdefULL, 0x2a6b003405ac8df8ULL, 0x2a6b003405ac8df8ULL, 0xcd4ed665f9fb649fULL },
    { "len=81    seed=0x0000000000000000", 81, 0x0000000000000000ULL, 0x4cf17317bc12b414ULL, 0x7d9f0e65ad34fed9ULL, 0xbb5a1a7d22e6577eULL },
    { "len=81    seed=0xdeadbeefcafebabe", 81, 0xdeadbeefcafebabeULL, 0x026d981abdadf712ULL, 0xabb843842705159eULL, 0x74d29064e60ec2ffULL },
    { "len=81    seed=0x0123456789abcdef", 81, 0x0123456789abcdefULL, 0x9a5328f4656f4d6dULL, 0x06da81f0581ffcb7ULL, 0x4fc5c96f195158f4ULL },
    { "len=96    seed=0x0000000000000000", 96, 0x0000000000000000ULL, 0x03024937660d09e3ULL, 0x9fb43a5c3fd18555ULL, 0xe3fa9db0a593b126ULL },
    { "len=96    seed=0xdeadbeefcafebabe", 96, 0xdeadbeefcafebabeULL, 0x4470e36da57a0b85ULL, 0x582b005dfb374e5dULL, 0x7a1741e1a3373bb8ULL },
    { "len=96    seed=0x0123456789abcdef", 96, 0x0123456789abcdefULL, 0xa0975a7763f4cd8cULL, 0xefb9fa2224558d39ULL, 0x3cf535d20248b201ULL },
    { "len=111   seed=0x0000000000000000", 111, 0x0000000000000000ULL, 0x3d9a4c27cbb7d0e1ULL, 0x5822108eaf2217e8ULL, 0x42f2f55ab2161990ULL },
    { "len=111   seed=0xdeadbeefcafebabe", 111, 0xdeadbeefcafebabeULL, 0x101c95e17856d1b2ULL, 0xf2a0c86acdf038ccULL, 0x22915f52a048edd4ULL },
    { "len=111   seed=0x0123456789abcdef", 111, 0x0123456789abcdefULL, 0xf3528b27ae2afaa4ULL, 0xb521424163bae487ULL, 0x320a12f451e34568ULL },
    { "len=112   seed=0x0000000000000000", 112, 0x0000000000000000ULL, 0xf8c7e88e1335f998ULL, 0x45f3d567aa756037ULL, 0xb16e42fbc6dc2b8eULL },
    { "len=112   seed=0xdeadbeefcafebabe", 112, 0xdeadbeefcafebabeULL, 0x64b354b739ea1c45ULL, 0x70088aa1466fa04dULL, 0xe06a90668e02edb2ULL },
    { "len=112   seed=0x0123456789abcdef", 112, 0x0123456789abcdefULL, 0xbdb9626c326e9f92ULL, 0xf078def0a9495589ULL, 0x642f62729f8e6e3eULL },
    { "len=113   seed=0x0000000000000000", 113, 0x0000000000000000ULL, 0x10d240519c63662fULL, 0x669e57eb565d378fULL, 0xe834411c5e74bb99ULL },
    { "len=113   seed=0xdeadbeefcafebabe", 113, 0xdeadbeefcafebabeULL, 0x1fd7b3e554505ae9ULL, 0xe0ebcf50c3aefc49ULL, 0x4b798df7ac4772e3ULL },
    { "len=113   seed=0x0123456789abcdef", 113, 0x0123456789abcdefULL, 0x39e8621e94acf6e1ULL, 0x5b7d5bde531eb582ULL, 0xeafdf19319ac686eULL },
    { "len=200   seed=0x0000000000000000", 200, 0x0000000000000000ULL, 0x7a41c1ea8ff549feULL, 0x51b4dbf3ff007206ULL, 0x6e62d59f66cb3778ULL },
    { "len=200   seed=0xdeadbeefcafebabe", 200, 0xdeadbeefcafebabeULL, 0x304494e0931e71ceULL, 0x5e00092cb4fdfd18ULL, 0x1f3bc3b3d03195a2ULL },
    { "len=200   seed=0x0123456789abcdef", 200, 0x0123456789abcdefULL, 0x8c18d6cf7c165924ULL, 0xe5a97f1fe567c997ULL, 0x5b5dde2b9b880c60ULL },
    { "len=224   seed=0x0000000000000000", 224, 0x0000000000000000ULL, 0x4650a27367d49f49ULL, 0xc04715f2e5a87856ULL, 0x3b875b255b9e4afdULL },
    { "len=224   seed=0xdeadbeefcafebabe", 224, 0xdeadbeefcafebabeULL, 0xa40e943e604da5b9ULL, 0x1ede760135c7eb44ULL, 0x709a991efc8e55c2ULL },
    { "len=224   seed=0x0123456789abcdef", 224, 0x0123456789abcdefULL, 0xe9a955aebdb69dfbULL, 0x0731c971cca58599ULL, 0xcf15d2b7b00540ebULL },
    { "len=225   seed=0x0000000000000000", 225, 0x0000000000000000ULL, 0x2cf3e0bd3e404d75ULL, 0x4da874d863390da3ULL, 0xdb2521613ee9f2b4ULL },
    { "len=225   seed=0xdeadbeefcafebabe", 225, 0xdeadbeefcafebabeULL, 0xc87791e3b9482eb8ULL, 0x986083e83606f047ULL, 0x7081f045d650620eULL },
    { "len=225   seed=0x0123456789abcdef", 225, 0x0123456789abcdefULL, 0x8c9c89b4fd8082dcULL, 0xd72bed9bab6c3898ULL, 0x425d2d255e901856ULL },
    { "len=1024  seed=0x0000000000000000", 1024, 0x0000000000000000ULL, 0x21cf5a8e9af78a6bULL, 0xaf5e79ca397d04d0ULL, 0xe2771548c168dbfeULL },
    { "len=1024  seed=0xdeadbeefcafebabe", 1024, 0xdeadbeefcafebabeULL, 0x5f45f34680c0fddcULL, 0x7e80b6e83fa4e8bdULL, 0xb007c901450ce259ULL },
    { "len=1024  seed=0x0123456789abcdef", 1024, 0x0123456789abcdefULL, 0x4f9d6336f6ef5f35ULL, 0xe7a1fcf15ba45082ULL, 0x29b1584c17f7a404ULL },
    { "len=8192  seed=0x0000000000000000", 8192, 0x0000000000000000ULL, 0x1367b5525e5e22c2ULL, 0xaba193427d48b843ULL, 0xaf0377bd99e3f6dfULL },
    { "len=8192  seed=0xdeadbeefcafebabe", 8192, 0xdeadbeefcafebabeULL, 0x835a7bfba382abadULL, 0xecdafd120a31f28dULL, 0xe726f9a70613e635ULL },
    { "len=8192  seed=0x0123456789abcdef", 8192, 0x0123456789abcdefULL, 0x0a5293e0ed1a148dULL, 0xcba262497cc854b5ULL, 0x8ed157640f33b5f8ULL },
    { "len=65536 seed=0x0000000000000000", 65536, 0x0000000000000000ULL, 0x45b901e6da9756e9ULL, 0xd499e60cbdf21d10ULL, 0x414f4670aedd4d7bULL },
    { "len=65536 seed=0xdeadbeefcafebabe", 65536, 0xdeadbeefcafebabeULL, 0x14d9eb4a4586cdb1ULL, 0x486cfec2d0215f3aULL, 0xdc8f7952d6bc7f36ULL },
    { "len=65536 seed=0x0123456789abcdef", 65536, 0x0123456789abcdefULL, 0x5d0a3548d7d9523bULL, 0x41aa097c96d1bc68ULL, 0xc6f035dc930c0007ULL },
};
/* clang-format on */

static constexpr size_t rapidhashTestVectorsCount =
    sizeof(rapidhashTestVectorsTable) / sizeof(rapidhashTestVectorsTable[0]);

/* -------------------------------------------------------------------------
 * PROTECTED-mode rapidhash test vectors.
 *
 * Captured from the upstream reference with -DRAPIDHASH_PROTECTED defined.
 * The PROTECTED build redefines rapid_mum() to XOR the multiply result
 * into A/B instead of overwriting them, so outputs differ from the FAST
 * table above. Same struct shape (rapidhashTestVector), same
 * (length, seed) cross-product, same "deterministic-fill-v1" inputs.
 *
 * Lengths (29):  0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 32, 48, 49, 64,
 *                79, 80, 81, 96, 111, 112, 113, 200, 224, 225, 1024,
 *                8192, 65536.
 * Seeds  (3):    0, 0xdeadbeefcafebabe, 0x0123456789abcdef.
 * ------------------------------------------------------------------------- */

/* clang-format off */
static const rapidhashTestVector rapidhashProtectedTestVectorsTable[] = {
    { "len=0     seed=0x0000000000000000", 0, 0x0000000000000000ULL, 0x9c4986892b492226ULL, 0x9c4986892b492226ULL, 0x9c4986892b492226ULL },
    { "len=0     seed=0xdeadbeefcafebabe", 0, 0xdeadbeefcafebabeULL, 0xdff234625abbe252ULL, 0xdff234625abbe252ULL, 0xdff234625abbe252ULL },
    { "len=0     seed=0x0123456789abcdef", 0, 0x0123456789abcdefULL, 0xc32e2db04d973624ULL, 0xc32e2db04d973624ULL, 0xc32e2db04d973624ULL },
    { "len=1     seed=0x0000000000000000", 1, 0x0000000000000000ULL, 0xd64d314a5ab6bb86ULL, 0xd64d314a5ab6bb86ULL, 0xd64d314a5ab6bb86ULL },
    { "len=1     seed=0xdeadbeefcafebabe", 1, 0xdeadbeefcafebabeULL, 0x16311e1b8512cee6ULL, 0x16311e1b8512cee6ULL, 0x16311e1b8512cee6ULL },
    { "len=1     seed=0x0123456789abcdef", 1, 0x0123456789abcdefULL, 0xa49e3a875a0774a4ULL, 0xa49e3a875a0774a4ULL, 0xa49e3a875a0774a4ULL },
    { "len=2     seed=0x0000000000000000", 2, 0x0000000000000000ULL, 0xee2969f9f66454e3ULL, 0xee2969f9f66454e3ULL, 0xee2969f9f66454e3ULL },
    { "len=2     seed=0xdeadbeefcafebabe", 2, 0xdeadbeefcafebabeULL, 0x8b1204feca6302afULL, 0x8b1204feca6302afULL, 0x8b1204feca6302afULL },
    { "len=2     seed=0x0123456789abcdef", 2, 0x0123456789abcdefULL, 0xfc19a1914499d41eULL, 0xfc19a1914499d41eULL, 0xfc19a1914499d41eULL },
    { "len=3     seed=0x0000000000000000", 3, 0x0000000000000000ULL, 0xbc441cde4c9e26daULL, 0xbc441cde4c9e26daULL, 0xbc441cde4c9e26daULL },
    { "len=3     seed=0xdeadbeefcafebabe", 3, 0xdeadbeefcafebabeULL, 0xa57ce03d90ea90c2ULL, 0xa57ce03d90ea90c2ULL, 0xa57ce03d90ea90c2ULL },
    { "len=3     seed=0x0123456789abcdef", 3, 0x0123456789abcdefULL, 0xded8de5b0dfc0d79ULL, 0xded8de5b0dfc0d79ULL, 0xded8de5b0dfc0d79ULL },
    { "len=4     seed=0x0000000000000000", 4, 0x0000000000000000ULL, 0x56fcb3c570013e0eULL, 0x56fcb3c570013e0eULL, 0x56fcb3c570013e0eULL },
    { "len=4     seed=0xdeadbeefcafebabe", 4, 0xdeadbeefcafebabeULL, 0x072a10e153516901ULL, 0x072a10e153516901ULL, 0x072a10e153516901ULL },
    { "len=4     seed=0x0123456789abcdef", 4, 0x0123456789abcdefULL, 0xc61365731bcdef37ULL, 0xc61365731bcdef37ULL, 0xc61365731bcdef37ULL },
    { "len=5     seed=0x0000000000000000", 5, 0x0000000000000000ULL, 0x981327846c3d2662ULL, 0x981327846c3d2662ULL, 0x981327846c3d2662ULL },
    { "len=5     seed=0xdeadbeefcafebabe", 5, 0xdeadbeefcafebabeULL, 0xb4c7f210fe2c258fULL, 0xb4c7f210fe2c258fULL, 0xb4c7f210fe2c258fULL },
    { "len=5     seed=0x0123456789abcdef", 5, 0x0123456789abcdefULL, 0x342b61c2c7f70931ULL, 0x342b61c2c7f70931ULL, 0x342b61c2c7f70931ULL },
    { "len=7     seed=0x0000000000000000", 7, 0x0000000000000000ULL, 0x6a0de77774bb3819ULL, 0x6a0de77774bb3819ULL, 0x6a0de77774bb3819ULL },
    { "len=7     seed=0xdeadbeefcafebabe", 7, 0xdeadbeefcafebabeULL, 0x25becbcf0043f8abULL, 0x25becbcf0043f8abULL, 0x25becbcf0043f8abULL },
    { "len=7     seed=0x0123456789abcdef", 7, 0x0123456789abcdefULL, 0x78f09a20903a7ca6ULL, 0x78f09a20903a7ca6ULL, 0x78f09a20903a7ca6ULL },
    { "len=8     seed=0x0000000000000000", 8, 0x0000000000000000ULL, 0xb30f2277f313d2a0ULL, 0xb30f2277f313d2a0ULL, 0xb30f2277f313d2a0ULL },
    { "len=8     seed=0xdeadbeefcafebabe", 8, 0xdeadbeefcafebabeULL, 0xb8ba0ccfc483d546ULL, 0xb8ba0ccfc483d546ULL, 0xb8ba0ccfc483d546ULL },
    { "len=8     seed=0x0123456789abcdef", 8, 0x0123456789abcdefULL, 0xbc109882c0cc17c1ULL, 0xbc109882c0cc17c1ULL, 0xbc109882c0cc17c1ULL },
    { "len=9     seed=0x0000000000000000", 9, 0x0000000000000000ULL, 0x12d26fdaf409e62eULL, 0x12d26fdaf409e62eULL, 0x12d26fdaf409e62eULL },
    { "len=9     seed=0xdeadbeefcafebabe", 9, 0xdeadbeefcafebabeULL, 0x4620b9d2732fe040ULL, 0x4620b9d2732fe040ULL, 0x4620b9d2732fe040ULL },
    { "len=9     seed=0x0123456789abcdef", 9, 0x0123456789abcdefULL, 0xcec0c6247fe4ab54ULL, 0xcec0c6247fe4ab54ULL, 0xcec0c6247fe4ab54ULL },
    { "len=15    seed=0x0000000000000000", 15, 0x0000000000000000ULL, 0x55062a2d174ce5e9ULL, 0x55062a2d174ce5e9ULL, 0x55062a2d174ce5e9ULL },
    { "len=15    seed=0xdeadbeefcafebabe", 15, 0xdeadbeefcafebabeULL, 0x481f3d71d6df36bcULL, 0x481f3d71d6df36bcULL, 0x481f3d71d6df36bcULL },
    { "len=15    seed=0x0123456789abcdef", 15, 0x0123456789abcdefULL, 0xc597f00a8338f914ULL, 0xc597f00a8338f914ULL, 0xc597f00a8338f914ULL },
    { "len=16    seed=0x0000000000000000", 16, 0x0000000000000000ULL, 0xc822cc6983dfbcf5ULL, 0xc822cc6983dfbcf5ULL, 0xc822cc6983dfbcf5ULL },
    { "len=16    seed=0xdeadbeefcafebabe", 16, 0xdeadbeefcafebabeULL, 0xeeb9072bb713ce87ULL, 0xeeb9072bb713ce87ULL, 0xeeb9072bb713ce87ULL },
    { "len=16    seed=0x0123456789abcdef", 16, 0x0123456789abcdefULL, 0x3b8cec91dab110d0ULL, 0x3b8cec91dab110d0ULL, 0x3b8cec91dab110d0ULL },
    { "len=17    seed=0x0000000000000000", 17, 0x0000000000000000ULL, 0xbf65dfc874ca8ff8ULL, 0xbf65dfc874ca8ff8ULL, 0xbf65dfc874ca8ff8ULL },
    { "len=17    seed=0xdeadbeefcafebabe", 17, 0xdeadbeefcafebabeULL, 0x78acebf461e4a706ULL, 0x78acebf461e4a706ULL, 0x78acebf461e4a706ULL },
    { "len=17    seed=0x0123456789abcdef", 17, 0x0123456789abcdefULL, 0x005a4c88864490ecULL, 0x005a4c88864490ecULL, 0x005a4c88864490ecULL },
    { "len=32    seed=0x0000000000000000", 32, 0x0000000000000000ULL, 0xf814684313047595ULL, 0xf814684313047595ULL, 0xf814684313047595ULL },
    { "len=32    seed=0xdeadbeefcafebabe", 32, 0xdeadbeefcafebabeULL, 0xecf04db19489b0ddULL, 0xecf04db19489b0ddULL, 0xecf04db19489b0ddULL },
    { "len=32    seed=0x0123456789abcdef", 32, 0x0123456789abcdefULL, 0x3a1161f6119ebf5dULL, 0x3a1161f6119ebf5dULL, 0x3a1161f6119ebf5dULL },
    { "len=48    seed=0x0000000000000000", 48, 0x0000000000000000ULL, 0x0e2c3daf6400f711ULL, 0x0e2c3daf6400f711ULL, 0x0e2c3daf6400f711ULL },
    { "len=48    seed=0xdeadbeefcafebabe", 48, 0xdeadbeefcafebabeULL, 0xf2fb925934637751ULL, 0xf2fb925934637751ULL, 0xf2fb925934637751ULL },
    { "len=48    seed=0x0123456789abcdef", 48, 0x0123456789abcdefULL, 0x83dbe94aab0e147eULL, 0x83dbe94aab0e147eULL, 0x83dbe94aab0e147eULL },
    { "len=49    seed=0x0000000000000000", 49, 0x0000000000000000ULL, 0xe73bd840566540f4ULL, 0xe73bd840566540f4ULL, 0x7d10a3539e11f156ULL },
    { "len=49    seed=0xdeadbeefcafebabe", 49, 0xdeadbeefcafebabeULL, 0xc0da4fc2fe89d376ULL, 0xc0da4fc2fe89d376ULL, 0xf359c150f666939fULL },
    { "len=49    seed=0x0123456789abcdef", 49, 0x0123456789abcdefULL, 0xe0e19b5be84b918cULL, 0xe0e19b5be84b918cULL, 0x02141e0f9a16fca4ULL },
    { "len=64    seed=0x0000000000000000", 64, 0x0000000000000000ULL, 0xa20c2adf8775e603ULL, 0xa20c2adf8775e603ULL, 0x55f1740e75c96cb0ULL },
    { "len=64    seed=0xdeadbeefcafebabe", 64, 0xdeadbeefcafebabeULL, 0x8f10066a83654132ULL, 0x8f10066a83654132ULL, 0xf764afaf45b05268ULL },
    { "len=64    seed=0x0123456789abcdef", 64, 0x0123456789abcdefULL, 0x02d697f3ad9dd3d5ULL, 0x02d697f3ad9dd3d5ULL, 0x7c21e7e1464cd002ULL },
    { "len=79    seed=0x0000000000000000", 79, 0x0000000000000000ULL, 0x662a44e93a0b4ad6ULL, 0x662a44e93a0b4ad6ULL, 0x0a304fd463d07c9fULL },
    { "len=79    seed=0xdeadbeefcafebabe", 79, 0xdeadbeefcafebabeULL, 0xce1e5a60c1877f03ULL, 0xce1e5a60c1877f03ULL, 0x7155d0d247f00db2ULL },
    { "len=79    seed=0x0123456789abcdef", 79, 0x0123456789abcdefULL, 0xe4cdcbe50e57668bULL, 0xe4cdcbe50e57668bULL, 0x0e816d3e155c6182ULL },
    { "len=80    seed=0x0000000000000000", 80, 0x0000000000000000ULL, 0x3241c0e7bf504f30ULL, 0x3241c0e7bf504f30ULL, 0x84281fa1a8e9d2a0ULL },
    { "len=80    seed=0xdeadbeefcafebabe", 80, 0xdeadbeefcafebabeULL, 0x7db37afc2f701866ULL, 0x7db37afc2f701866ULL, 0x4b07aab3edbf3a35ULL },
    { "len=80    seed=0x0123456789abcdef", 80, 0x0123456789abcdefULL, 0x2f90434037feb652ULL, 0x2f90434037feb652ULL, 0xf6c4f1de2b8c116dULL },
    { "len=81    seed=0x0000000000000000", 81, 0x0000000000000000ULL, 0x9dbef1fdae6f8067ULL, 0x4c645b11e750c7c8ULL, 0x7b72e006a582718aULL },
    { "len=81    seed=0xdeadbeefcafebabe", 81, 0xdeadbeefcafebabeULL, 0xc6a9bcf833fc90c2ULL, 0x141ca2f6ec0a7170ULL, 0xdad2739ceb28cb1fULL },
    { "len=81    seed=0x0123456789abcdef", 81, 0x0123456789abcdefULL, 0x9692ba0b955fa648ULL, 0xfb30cd52ce69930fULL, 0xe442d1ba515506dfULL },
    { "len=96    seed=0x0000000000000000", 96, 0x0000000000000000ULL, 0x1e1ba1ac89bda584ULL, 0xb4aeacc27ee574e5ULL, 0x11a7193ff4a2cf63ULL },
    { "len=96    seed=0xdeadbeefcafebabe", 96, 0xdeadbeefcafebabeULL, 0x07cb9ee2fc8f9337ULL, 0xed3bfe70bb2464c7ULL, 0xbb32a1e530961244ULL },
    { "len=96    seed=0x0123456789abcdef", 96, 0x0123456789abcdefULL, 0xa01eca7f408bd60bULL, 0xc562422ae21bbcebULL, 0x9c2a9dfec7c071c0ULL },
    { "len=111   seed=0x0000000000000000", 111, 0x0000000000000000ULL, 0x3521770beeee79b1ULL, 0x391642c9e2160a0dULL, 0x72ad2d9665a719ebULL },
    { "len=111   seed=0xdeadbeefcafebabe", 111, 0xdeadbeefcafebabeULL, 0x16ef23514ad60257ULL, 0x5c87183ade0e1a91ULL, 0x806b7533652c008aULL },
    { "len=111   seed=0x0123456789abcdef", 111, 0x0123456789abcdefULL, 0x3d60d2450251a531ULL, 0x745d07c5c69d59f8ULL, 0x1e8b1c12210b1a66ULL },
    { "len=112   seed=0x0000000000000000", 112, 0x0000000000000000ULL, 0x874689a3881f2a68ULL, 0xc728a7181a584594ULL, 0x712fd8da829c588bULL },
    { "len=112   seed=0xdeadbeefcafebabe", 112, 0xdeadbeefcafebabeULL, 0x2bd1f935ed7a2444ULL, 0x7fc7864c650de490ULL, 0xebc2798df9c181d5ULL },
    { "len=112   seed=0x0123456789abcdef", 112, 0x0123456789abcdefULL, 0x589ba6246ddf591fULL, 0xea71afa8c102f3e8ULL, 0x9c0de306cdaa690cULL },
    { "len=113   seed=0x0000000000000000", 113, 0x0000000000000000ULL, 0x64574fa9deedfdb8ULL, 0xd3d7acfdb0c777a3ULL, 0x15d80a76fac1efb0ULL },
    { "len=113   seed=0xdeadbeefcafebabe", 113, 0xdeadbeefcafebabeULL, 0x7a037de11b925d5eULL, 0x3893f8e5bd835e4aULL, 0x40f235e1d0ec00dbULL },
    { "len=113   seed=0x0123456789abcdef", 113, 0x0123456789abcdefULL, 0xf7be48de0bee43d9ULL, 0x8a0d709b6c7fc9e8ULL, 0xb2774e9beb53fa75ULL },
    { "len=200   seed=0x0000000000000000", 200, 0x0000000000000000ULL, 0x6bfd4b8b874c8349ULL, 0x8c595962165ea924ULL, 0x0bbe7a0f005cb2c2ULL },
    { "len=200   seed=0xdeadbeefcafebabe", 200, 0xdeadbeefcafebabeULL, 0xc4b31b6ab1441589ULL, 0x0df04d98874e5580ULL, 0x4c75bebb84dcf8c5ULL },
    { "len=200   seed=0x0123456789abcdef", 200, 0x0123456789abcdefULL, 0x0922de0b458cdd29ULL, 0x7e7e8de7204cee58ULL, 0xedb5cd89db1c14eeULL },
    { "len=224   seed=0x0000000000000000", 224, 0x0000000000000000ULL, 0x87af96def1a68564ULL, 0x0d9f72bad6e66e61ULL, 0x54819f6046e89becULL },
    { "len=224   seed=0xdeadbeefcafebabe", 224, 0xdeadbeefcafebabeULL, 0x064c40c0cf7f6789ULL, 0x68e36e335a7fde07ULL, 0x936d67673ad96014ULL },
    { "len=224   seed=0x0123456789abcdef", 224, 0x0123456789abcdefULL, 0x74bc99d60df1c3faULL, 0x38e7b39cf8864c56ULL, 0xd78a13b93578e6dfULL },
    { "len=225   seed=0x0000000000000000", 225, 0x0000000000000000ULL, 0x22bbb720ba61a09eULL, 0x92ff35c81ee9ff79ULL, 0x8c1f75a6650fe5e2ULL },
    { "len=225   seed=0xdeadbeefcafebabe", 225, 0xdeadbeefcafebabeULL, 0x9100bf4941aecb0fULL, 0x008043451fc39c23ULL, 0x238e2e3c48c497b3ULL },
    { "len=225   seed=0x0123456789abcdef", 225, 0x0123456789abcdefULL, 0xedf607a53e17d090ULL, 0xf47f92aa7b8d56f2ULL, 0x167394a58a2ef399ULL },
    { "len=1024  seed=0x0000000000000000", 1024, 0x0000000000000000ULL, 0x2621114d3cf7366eULL, 0xfe57e7f2869125f6ULL, 0x530ae2f14ebf96e8ULL },
    { "len=1024  seed=0xdeadbeefcafebabe", 1024, 0xdeadbeefcafebabeULL, 0x86340067020a3891ULL, 0xce03ef86cf34a627ULL, 0xd0be5a3b97c9b511ULL },
    { "len=1024  seed=0x0123456789abcdef", 1024, 0x0123456789abcdefULL, 0xf39e52abf03f6dd0ULL, 0x4967688e7e78fdb1ULL, 0xb77ba33b12fa5dadULL },
    { "len=8192  seed=0x0000000000000000", 8192, 0x0000000000000000ULL, 0x7b0bd0830b2b90b0ULL, 0xcb9e957ed0666493ULL, 0x8b8704839c8533f2ULL },
    { "len=8192  seed=0xdeadbeefcafebabe", 8192, 0xdeadbeefcafebabeULL, 0x61c7d1b2a9fcc469ULL, 0x9cd3396f2a8b5028ULL, 0x0edec66d0e6a0a1dULL },
    { "len=8192  seed=0x0123456789abcdef", 8192, 0x0123456789abcdefULL, 0x74d63e565b368dd8ULL, 0x761e27a0bc95b99aULL, 0xe54f614c8d67e4bfULL },
    { "len=65536 seed=0x0000000000000000", 65536, 0x0000000000000000ULL, 0xe57e3c99aff74c8fULL, 0x63e0fc79759f9418ULL, 0x864b8ac8d039c438ULL },
    { "len=65536 seed=0xdeadbeefcafebabe", 65536, 0xdeadbeefcafebabeULL, 0xb5ad65833ab282e2ULL, 0x109aa3a0a38cc93bULL, 0xa9f9bb2c819d4d5eULL },
    { "len=65536 seed=0x0123456789abcdef", 65536, 0x0123456789abcdefULL, 0xdd9efc09fdcd36e4ULL, 0xaf6012738f6efb6fULL, 0x860b1097a55e65c6ULL },
};
/* clang-format on */

static constexpr size_t rapidhashProtectedTestVectorsCount =
    sizeof(rapidhashProtectedTestVectorsTable) /
    sizeof(rapidhashProtectedTestVectorsTable[0]);

/* -------------------------------------------------------------------------
 * make_secret() KAT vectors.
 *
 * The upstream secret.h ships a make_secret(seed, secret[4]) helper that
 * deterministically derives a 4-word custom secret table from a 64-bit
 * seed. The algorithm walks wyrand() and rejects words that aren't
 * (odd, prime, popcount-31-from-all-prior-words) so it is sensitive to
 * any drift in rapid_mix / wyrand semantics. Capturing the exact 4-word
 * output for a handful of seeds gives us a stable cross-check on the
 * datakit port of that helper.
 *
 * Only secret[0..3] are written by make_secret() upstream; we capture
 * all four words.
 * ------------------------------------------------------------------------- */

typedef struct rapidhashSecretTestVector {
    uint64_t seed;
    uint64_t expected[4];
} rapidhashSecretTestVector;

/* clang-format off */
static const rapidhashSecretTestVector rapidhashSecretTestVectorsTable[] = {
    { 0x0000000000000000ULL, { 0x39d43c5c4e3a724bULL, 0x6596e14753cca38bULL, 0xc68d954b2b339353ULL, 0x96b4a6e45c65aa55ULL } },
    { 0x0000000000000001ULL, { 0x0ff09999359563a5ULL, 0x2d6c35b48de81b2bULL, 0xd2ca93c527b49c2bULL, 0x47c3660fc5e1c9b1ULL } },
    { 0xdeadbeefcafebabeULL, { 0x1d6a55e1e41bccb1ULL, 0x59668e723caa1b53ULL, 0x3a1eb893d4c9d1c5ULL, 0xa5551b39cca96659ULL } },
    { 0x0123456789abcdefULL, { 0x65962e71a35369c3ULL, 0x369c5cd26927aa2dULL, 0x53d82d4b5c5655a9ULL, 0xb88bac27cce1b88bULL } },
    { 0xffffffffffffffffULL, { 0x550fc6c5d4aa6a95ULL, 0xd16ce139c6e1b44dULL, 0xb239d83c653c4ec5ULL, 0x8ee8d4c3c9999ca5ULL } },
};
/* clang-format on */

static constexpr size_t rapidhashSecretTestVectorsCount =
    sizeof(rapidhashSecretTestVectorsTable) /
    sizeof(rapidhashSecretTestVectorsTable[0]);
