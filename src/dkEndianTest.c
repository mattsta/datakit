#include "dkEndian.h"

#ifdef DATAKIT_TEST
#include "ctest.h"

#include <inttypes.h>

/* Every width and order against bytes placed one at a time: the value's
 * byte i (counting from the least significant) sits at offset i in little
 * endian and at offset width/8 - 1 - i in big endian; loads and stores
 * at every misalignment in a buffer */
#define DKENDIAN_CHECK(width, v)                                               \
    do {                                                                       \
        const uint##width##_t x_ = (uint##width##_t)(v);                       \
        constexpr size_t n_ = (width) / 8;                                     \
        uint8_t le_[n_], be_[n_];                                              \
        for (size_t i = 0; i < n_; i++) {                                      \
            le_[i] = (uint8_t)(x_ >> (8 * i));                                 \
            be_[n_ - 1 - i] = (uint8_t)(x_ >> (8 * i));                        \
        }                                                                      \
        for (size_t off = 0; off < 8; off++) {                                 \
            uint8_t buf[16 + 8] = {};                                          \
            memcpy(buf + off, le_, n_);                                        \
            if (dkLoadLe##width(buf + off) != x_) {                            \
                ERR("dkLoadLe" #width " at +%zu", off);                        \
            }                                                                  \
            memcpy(buf + off, be_, n_);                                        \
            if (dkLoadBe##width(buf + off) != x_) {                            \
                ERR("dkLoadBe" #width " at +%zu", off);                        \
            }                                                                  \
            uint8_t out[16 + 8] = {};                                          \
            dkStoreLe##width(out + off, x_);                                   \
            if (memcmp(out + off, le_, n_) != 0) {                             \
                ERR("dkStoreLe" #width " at +%zu", off);                       \
            }                                                                  \
            dkStoreBe##width(out + off, x_);                                   \
            if (memcmp(out + off, be_, n_) != 0) {                             \
                ERR("dkStoreBe" #width " at +%zu", off);                       \
            }                                                                  \
            if (out[off + n_] != 0 || (off && out[off - 1] != 0)) {            \
                ERR("dkStore" #width " wrote outside its bytes at +%zu", off); \
            }                                                                  \
        }                                                                      \
    } while (0)

int dkEndianTest(int, char *[]) {
    int err = 0;

    TEST("load and store at 16, 32 and 64 bits, both orders, any "
         "alignment") {
        uint64_t s = 0x0123456789ABCDEFu;
        for (int round = 0; round < 200; round++) {
            DKENDIAN_CHECK(16, s);
            DKENDIAN_CHECK(32, s);
            DKENDIAN_CHECK(64, s);
            dkSplitmix64Next(&s);
        }
    }

    TEST("known bytes: 0x01020304 is 04 03 02 01 little, 01 02 03 04 big") {
        const uint8_t le[] = {4, 3, 2, 1};
        const uint8_t be[] = {1, 2, 3, 4};
        if (dkLoadLe32(le) != 0x01020304u || dkLoadBe32(be) != 0x01020304u) {
            ERR("32-bit known bytes");
        }
        if (dkLoadLe64((const uint8_t[]){8, 7, 6, 5, 4, 3, 2, 1}) !=
                UINT64_C(0x0102030405060708) ||
            dkLoadBe16((const uint8_t[]){0xAB, 0xCD}) != 0xABCD) {
            ERR("64/16-bit known bytes");
        }
    }

    TEST_FINAL_RESULT;
}
#endif
