#include "databoxLinear.h"
#include "../deps/varint/src/varint.h"
#include "../deps/varint/src/varintExternal.h" /* user integers */

#include "conformLittleEndian.h"
#include "float16.h"

typedef enum databoxLinearType {
    /* NEW TYPES MUST BE APPENDED AFTER THE END.
     * THIS IS A STATIC, ON-DISK, MULTI-MACHINE FILE FORMAT.
     * NEW TYPES MUST NEVER CHANGE THE ORDER OR INDEXING OF EXISTING TYPES. */
    DL_INVALID = 0,
    DL_BYTES = 1,

    DL_NEG_8B = 2,
    DL_UINT_8B = 3,
    DL_NEG_16B = 4,
    DL_UINT_16B = 5,
    DL_NEG_24B = 6,
    DL_UINT_24B = 7,
    DL_NEG_32B = 8,
    DL_UINT_32B = 9,
    DL_NEG_40B = 10,
    DL_UINT_40B = 11,
    DL_NEG_48B = 12,
    DL_UINT_48B = 13,
    DL_NEG_56B = 14,
    DL_UINT_56B = 15,
    DL_NEG_64B = 16,
    DL_UINT_64B = 17,

    DL_REAL_16B = 18,
    DL_REAL_32B = 19,
    DL_REAL_64B = 20,

    DL_TRUE = 21,
    DL_FALSE = 22,
    DL_NULL = 23,

    /* NEW TYPES MUST BE APPENDED, NOT PREPENDED.
     * THIS IS A STATIC, ON-DISK, MULTI-MACHINE FILE FORMAT.
     * NEW TYPES MUST NEVER CHANGE THE ORDER OR INDEXING OF EXISTING TYPES. */
} databoxLinearType;

/* Integer encoding step is the number of elements between successive
 * same-signed integer encodings.
 * (which is 2 since the steps are (negative, unsigned),
 * but this is more programatically explicit) */
#define DL_INTEGER_ENCODING_STEP (DL_UINT_16B - DL_UINT_8B)

#define DL_WIDTH_FROM_ENCODING(encoding)                                       \
    ((((encoding) - DL_NEG_8B) / DL_INTEGER_ENCODING_STEP) + 1)

/* ====================================================================
 * Number Prep
 * ==================================================================== */
DK_STATIC DK_FN_CONST databoxLinearType
databoxLinearEncodingUnsigned(uint64_t value) {
    /* Determine the smallest encoding for 'value' */

    databoxLinearType encoding = DL_UINT_8B;

    /* The "BYTESUSED" macro doesn't work for value '0', so anything
     * less than one byte, just use the default value. */
    if (value <= UINT8_MAX) {
        return encoding;
    }

#if 0
    while ((value >>= 8) != 0) {
        encoding += DL_INTEGER_ENCODING_STEP; /* 1 gap between unsigned types */
    }
#else
    encoding += (DK_BYTESUSED(value) - 1) * DL_INTEGER_ENCODING_STEP;
#endif

    return encoding;
}

/* increase the negative number by one because we don't store
 * signed zero and can use '0' to store '-1', etc. */
#define SIGNED_PREPARE(v) ((v) + 1)

/* restore the sign bit then go one lower to reverse SIGNED_PREPARE */
#define SIGNED_RESTORE(v) (-(v) - 1)

DK_STATIC
DK_FN_CONST databoxLinearType databoxLinearEncodingSigned(int64_t value) {
    /* Determine the smallest encoding for 'value' */

    if (value < 0) {
        /* Convert signed to unsigned in proper range */
        /* Minus one because we don't need to store a signed zero,
         * so we adjust all values by one. */
        const uint64_t converted = DK_INT64_TO_UINT64(value) - 1;

        /* To save us from having to compare 16 ranges, just:
         *   - turn negative number positive (unsigned)
         *   - compare in unsigned range
         *   - convert unsigned type to negative type */
        /* Our negative type IDs are one minus their unsigned counterparts. */
        return databoxLinearEncodingUnsigned(converted) - 1;
    }

    return databoxLinearEncodingUnsigned(value);
}

DK_STATIC databoxLinearType databoxLinearEncodingFloat(float value) {
    /* If conversion of f32->f16->f32 retains the value, we can
     * store the entire value as 2 bytes instead of 4. */
    if (float16Decode(float16Encode(value)) == value) {
        return DL_REAL_16B;
    }

    return DL_REAL_32B;
}

DK_STATIC databoxLinearType databoxLinearEncodingDouble(double value) {
    /* Attempt to encode double to float with no loss of precision */
    if ((double)(float)value == value) {
        /* Sucess! Now try to encode to REAL_16 too. */
        return databoxLinearEncodingFloat(value);
    }

    return DL_REAL_64B;
}

DK_STATIC void databoxLinearSaveFloatHalf(uint8_t *const target,
                                          const float value) {
    const uint16_t encoded = float16Encode(value);
    memcpy(target, &encoded, sizeof(encoded));
    conformToLittleEndian16(*target);
}

DK_STATIC void databoxLinearSaveFloat(uint8_t *const target,
                                      const float value) {
    memcpy(target, &value, sizeof(value));
    conformToLittleEndian32(*target);
}

/* Store double 'value' at 'fe' */
DK_STATIC void databoxLinearSaveDouble(uint8_t *const target,
                                       const double value) {
    memcpy(target, &value, sizeof(value));
    conformToLittleEndian64(*target);
}

/* ====================================================================
 * Writing
 * ==================================================================== */
/* Returns linear encoding size of the value NOT including the type byte */
ssize_t databoxLinearEncode(const databox *restrict const box,
                            databoxLinear *restrict const boxl) {
    void *start = &boxl->data;

    /* Step 1: Get type from current box */
    /* Step 2: Set type of Linear based on actual size required */
    switch (box->type) {
    case DATABOX_UNSIGNED_64:
        boxl->type = databoxLinearEncodingUnsigned(box->data.u);
        break;
    case DATABOX_SIGNED_64:
        boxl->type = databoxLinearEncodingSigned(box->data.i);
        break;
    case DATABOX_FLOAT_32:
        boxl->type = databoxLinearEncodingFloat(box->data.f32);
        boxl->data.f32 = box->data.f32;
        break;
    case DATABOX_DOUBLE_64:
        boxl->type = databoxLinearEncodingDouble(box->data.d64);
        /* If we down-converted to 32 or 16 bits, we must convert the
         * double to a float because we cast the float directly below. */
        if (boxl->type != DL_REAL_64B) {
            boxl->data.f32 = box->data.d64;
        }
        break;
    case DATABOX_TRUE:
        boxl->type = DL_TRUE;
        return 0;
    case DATABOX_FALSE:
        boxl->type = DL_FALSE;
        return 0;
    case DATABOX_NULL:
        boxl->type = DL_NULL;
        return 0;
    }

    /* Step 3: Write required bytes and return length of data */
    switch (boxl->type) {
    case DL_NEG_8B:
    case DL_NEG_16B:
    case DL_NEG_24B:
    case DL_NEG_32B:
    case DL_NEG_40B:
    case DL_NEG_48B:
    case DL_NEG_56B:
    case DL_NEG_64B: {
        const varintWidth width = DL_WIDTH_FROM_ENCODING(boxl->type);
        const uint64_t prepared = -SIGNED_PREPARE(box->data.i);
        varintExternalPutFixedWidthQuick_(start, prepared, width);
        return width;
    }
    case DL_UINT_8B:
    case DL_UINT_16B:
    case DL_UINT_24B:
    case DL_UINT_32B:
    case DL_UINT_40B:
    case DL_UINT_48B:
    case DL_UINT_56B:
    case DL_UINT_64B: {
        /* Prevent unaligned read with this memcpy */
        //        uint64_t val = *(uint64_t *)data;
        //        memcpy(&val, data, sizeof(val));
        const varintWidth width = DL_WIDTH_FROM_ENCODING(boxl->type);
        varintExternalPutFixedWidthQuick_(start, box->data.u, width);
        return width;
    }
    case DL_REAL_16B:
        /* yes, use 'boxl' because we set above when determining type */
        databoxLinearSaveFloatHalf(start, boxl->data.f32);
        return 2;
    case DL_REAL_32B:
        /* yes, use 'boxl' because we set above when determining type */
        databoxLinearSaveFloat(start, boxl->data.f32);
        return sizeof(float);
    case DL_REAL_64B:
        databoxLinearSaveDouble(start, box->data.d64);
        return sizeof(double);
    default:
        assert(0 && "unsupported databox type for linear encoding");
        return -1;
    }
}

/* ====================================================================
 * Reading
 * ==================================================================== */
static bool databoxLinearDecodeAbstract(const databoxLinearType type,
                                        const void *restrict start,
                                        databox *restrict box) {
    /* Clear box data so we can write into and shift around clean bytes */
    box->data.u = 0;

    switch (type) {
    case DL_UINT_8B:
    case DL_UINT_16B:
    case DL_UINT_24B:
    case DL_UINT_32B:
    case DL_UINT_40B:
    case DL_UINT_48B:
    case DL_UINT_56B:
    case DL_UINT_64B:
        varintExternalGetQuick_(start, DL_WIDTH_FROM_ENCODING(type),
                                box->data.u64);
        box->type = DATABOX_UNSIGNED_64;
        break;
    case DL_NEG_8B:
    case DL_NEG_16B:
    case DL_NEG_24B:
    case DL_NEG_32B:
    case DL_NEG_40B:
    case DL_NEG_48B:
    case DL_NEG_56B:
    case DL_NEG_64B:
        varintExternalGetQuick_(start, DL_WIDTH_FROM_ENCODING(type),
                                box->data.i64);
        box->type = DATABOX_SIGNED_64;
        box->data.i64 =
            SIGNED_RESTORE(box->data.i64); /* restore sign and offset */
        break;
    case DL_REAL_16B: {
        /* Copy buffer back to our space,
         * bswap if necessary,
         * decode back to usable f32 */
        memcpy(&box->data.u16, start, 2);
        conformToLittleEndian16(box->data.u16);
        box->data.f32 = float16Decode(box->data.u16);
        box->type = DATABOX_FLOAT_32;
        break;
    }
    case DL_REAL_32B:
        memcpy(&box->data.f32, start, sizeof(box->data.f32));
        conformToLittleEndian32(box->data.f32);
        box->type = DATABOX_FLOAT_32;
        break;
    case DL_REAL_64B:
        memcpy(&box->data.d64, start, sizeof(box->data.d64));
        conformToLittleEndian64(box->data.d64);
        box->type = DATABOX_DOUBLE_64;
        break;
    case DL_TRUE:
        box->type = DATABOX_TRUE;
        break;
    case DL_FALSE:
        box->type = DATABOX_FALSE;
        break;
    case DL_NULL:
        box->type = DATABOX_NULL;
        break;
    default:
        /* Invalid type - cannot decode */
        assert(0 && "invalid databoxLinear type");
        return false;
    }

    return true;
}

bool databoxLinearDecode(const databoxLinear *restrict const boxl,
                         databox *restrict box) {
    return databoxLinearDecodeAbstract(boxl->type, &boxl->data, box);
}

bool databoxLinearDecodeParts(const uint_fast8_t type,
                              const void *restrict start,
                              databox *restrict box) {
    return databoxLinearDecodeAbstract(type, start, box);
}

#ifdef DATAKIT_TEST
#include "ctest.h"
#include <float.h>
#include <math.h>

/* Simple xorshift64* PRNG for reproducible fuzzing */
static uint64_t fuzzState = 0x123456789ABCDEF0ULL;
static uint64_t fuzzNext(void) {
    fuzzState ^= fuzzState >> 12;
    fuzzState ^= fuzzState << 25;
    fuzzState ^= fuzzState >> 27;
    return fuzzState * 0x2545F4914F6CDD1DULL;
}

/* Generate random value in range [0, max] */
static uint64_t fuzzRange(uint64_t max) {
    return fuzzNext() % (max + 1);
}

/* Generate random signed value */
static int64_t fuzzSigned(void) {
    uint64_t u = fuzzNext();
    return (int64_t)u;
}

/* Generate random float with various characteristics */
static float fuzzFloat(void) {
    uint32_t bits = (uint32_t)fuzzNext();
    float f;
    memcpy(&f, &bits, sizeof(f));
    /* Filter out NaN/Inf for now - test those separately */
    if (!isfinite(f)) {
        return (float)(fuzzNext() % 1000000) / 1000.0f;
    }
    return f;
}

/* Generate random double with various characteristics */
static double fuzzDouble(void) {
    uint64_t bits = fuzzNext();
    double d;
    memcpy(&d, &bits, sizeof(d));
    /* Filter out NaN/Inf for now */
    if (!isfinite(d)) {
        return (double)(fuzzNext() % 1000000000) / 1000000.0;
    }
    return d;
}

int databoxLinearTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    /* ----------------------------------------------------------------
     * Unsigned Integer Tests
     * ---------------------------------------------------------------- */

    TEST("unsigned integers - small values") {
        uint64_t values[] = {0, 1, 2, 127, 128, 255};
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            databox a = {.data.u = values[i], .type = DATABOX_UNSIGNED_64};
            databoxLinear dl = {0};
            ssize_t len = databoxLinearEncode(&a, &dl);

            if (len != 1) {
                ERR("Value %llu should encode to 1 byte, got %zd",
                    (unsigned long long)values[i], len);
            }

            databox b = {{0}};
            if (!databoxLinearDecode(&dl, &b)) {
                ERR("Failed to decode value %llu",
                    (unsigned long long)values[i]);
            }
            if (b.type != DATABOX_UNSIGNED_64) {
                ERR("Type mismatch for value %llu",
                    (unsigned long long)values[i]);
            }
            if (b.data.u != values[i]) {
                ERR("Value mismatch: expected %llu, got %llu",
                    (unsigned long long)values[i],
                    (unsigned long long)b.data.u);
            }
        }
    }

    TEST("unsigned integers - boundary values") {
        /* Test at byte boundaries: 2^8, 2^16, 2^24, etc. */
        for (int bits = 8; bits <= 64; bits += 8) {
            uint64_t val = (bits == 64) ? UINT64_MAX : ((1ULL << bits) - 1);
            databox a = {.data.u = val, .type = DATABOX_UNSIGNED_64};
            databoxLinear dl = {0};
            ssize_t len = databoxLinearEncode(&a, &dl);

            int expectedLen = bits / 8;
            if (len != expectedLen) {
                ERR("Value 2^%d-1 should encode to %d bytes, got %zd", bits,
                    expectedLen, len);
            }

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);
            if (b.data.u != val) {
                ERR("Roundtrip failed for 2^%d-1", bits);
            }
        }
    }

    TEST("unsigned integers - UINT64_MAX") {
        databox a = {.data.u = UINT64_MAX, .type = DATABOX_UNSIGNED_64};
        databoxLinear dl = {0};
        ssize_t len = databoxLinearEncode(&a, &dl);

        if (len != 8) {
            ERR("UINT64_MAX should encode to 8 bytes, got %zd", len);
        }

        databox b = {{0}};
        databoxLinearDecode(&dl, &b);
        if (b.data.u != UINT64_MAX) {
            ERRR("UINT64_MAX roundtrip failed");
        }
    }

    /* ----------------------------------------------------------------
     * Signed Integer Tests
     * ---------------------------------------------------------------- */

    TEST("signed integers - small values") {
        int64_t values[] = {0, 1, -1, 127, -128, 128, -129};
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            databox a = {.data.i = values[i], .type = DATABOX_SIGNED_64};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);

            /* Positive values decode as unsigned */
            if (values[i] >= 0) {
                if (b.data.i != values[i]) {
                    ERR("Value mismatch for %lld", (long long)values[i]);
                }
            } else {
                if (b.type != DATABOX_SIGNED_64) {
                    ERR("Negative %lld should decode as SIGNED",
                        (long long)values[i]);
                }
                if (b.data.i != values[i]) {
                    ERR("Value mismatch: expected %lld, got %lld",
                        (long long)values[i], (long long)b.data.i);
                }
            }
        }
    }

    TEST("signed integers - INT64_MIN") {
        databox a = {.data.i = INT64_MIN, .type = DATABOX_SIGNED_64};
        databoxLinear dl = {0};
        ssize_t len = databoxLinearEncode(&a, &dl);

        if (len != 8) {
            ERR("INT64_MIN should encode to 8 bytes, got %zd", len);
        }

        databox b = {{0}};
        databoxLinearDecode(&dl, &b);
        if (b.type != DATABOX_SIGNED_64) {
            ERRR("INT64_MIN should decode as SIGNED");
        }
        if (b.data.i != INT64_MIN) {
            ERRR("INT64_MIN roundtrip failed");
        }
    }

    TEST("signed integers - INT64_MAX") {
        databox a = {.data.i = INT64_MAX, .type = DATABOX_SIGNED_64};
        databoxLinear dl = {0};
        ssize_t len = databoxLinearEncode(&a, &dl);

        if (len != 8) {
            ERR("INT64_MAX should encode to 8 bytes, got %zd", len);
        }

        databox b = {{0}};
        databoxLinearDecode(&dl, &b);
        /* INT64_MAX is positive, so it decodes as unsigned */
        if (b.data.u != (uint64_t)INT64_MAX) {
            ERRR("INT64_MAX roundtrip failed");
        }
    }

    /* ----------------------------------------------------------------
     * Float Tests
     * ---------------------------------------------------------------- */

    TEST("float - basic values") {
        float values[] = {0.0f, 1.0f, -1.0f, 0.5f, 3.14159f};
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            databox a = {.data.f32 = values[i], .type = DATABOX_FLOAT_32};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);

            if (b.type != DATABOX_FLOAT_32) {
                ERR("Float %g type mismatch", (double)values[i]);
            }
            if (b.data.f32 != values[i]) {
                ERR("Float roundtrip failed: expected %g, got %g",
                    (double)values[i], (double)b.data.f32);
            }
        }
    }

    TEST("float - float16 downgrade") {
        /* These values can be exactly represented in float16 */
        float values[] = {0.0f, 1.0f, 2.0f, 0.5f, -1.0f};
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            databox a = {.data.f32 = values[i], .type = DATABOX_FLOAT_32};
            databoxLinear dl = {0};
            ssize_t len = databoxLinearEncode(&a, &dl);

            if (len != 2) {
                ERR("Simple float %g should encode to 2 bytes (float16), got "
                    "%zd",
                    (double)values[i], len);
            }

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);
            if (b.data.f32 != values[i]) {
                ERR("Float16 roundtrip failed for %g", (double)values[i]);
            }
        }
    }

    /* ----------------------------------------------------------------
     * Double Tests
     * ---------------------------------------------------------------- */

    TEST("double - basic values") {
        double values[] = {0.0, 1.0, -1.0, 3.141592653589793};
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            databox a = {.data.d64 = values[i], .type = DATABOX_DOUBLE_64};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);

            /* Simple values downgrade to float32 or float16 */
            if (values[i] == 0.0 || values[i] == 1.0 || values[i] == -1.0) {
                if (b.type != DATABOX_FLOAT_32) {
                    ERR("Simple double %g should downgrade to float",
                        values[i]);
                }
            }

            /* Verify value survived */
            double result =
                (b.type == DATABOX_DOUBLE_64) ? b.data.d64 : (double)b.data.f32;
            if (result != values[i]) {
                ERR("Double roundtrip failed for %g", values[i]);
            }
        }
    }

    TEST("double - precision preservation") {
        /* This value cannot be represented as float32 */
        double val = 1.0000000000001;
        databox a = {.data.d64 = val, .type = DATABOX_DOUBLE_64};
        databoxLinear dl = {0};
        ssize_t len = databoxLinearEncode(&a, &dl);

        if (len != 8) {
            ERR("High-precision double should encode to 8 bytes, got %zd", len);
        }

        databox b = {{0}};
        databoxLinearDecode(&dl, &b);
        if (b.type != DATABOX_DOUBLE_64) {
            ERRR("High-precision double should remain DOUBLE_64");
        }
        if (b.data.d64 != val) {
            ERRR("High-precision double roundtrip failed");
        }
    }

    /* ----------------------------------------------------------------
     * Boolean and Null Tests
     * ---------------------------------------------------------------- */

    TEST("boolean and null types") {
        databoxType types[] = {DATABOX_TRUE, DATABOX_FALSE, DATABOX_NULL};
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            databox a = {.type = types[i]};
            databoxLinear dl = {0};
            ssize_t len = databoxLinearEncode(&a, &dl);

            if (len != 0) {
                ERR("Boolean/null should encode to 0 value bytes, got %zd",
                    len);
            }

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);
            if (b.type != types[i]) {
                ERR("Type roundtrip failed for type %d", types[i]);
            }
        }
    }

    /* ----------------------------------------------------------------
     * DecodeParts Tests
     * ---------------------------------------------------------------- */

    TEST("DecodeParts function") {
        /* Encode a value, then decode using DecodeParts */
        databox a = {.data.u = 12345, .type = DATABOX_UNSIGNED_64};
        databoxLinear dl = {0};
        databoxLinearEncode(&a, &dl);

        databox b = {{0}};
        bool ok = databoxLinearDecodeParts(dl.type, &dl.data, &b);
        if (!ok) {
            ERRR("DecodeParts failed");
        }
        if (b.data.u != 12345) {
            ERR("DecodeParts value mismatch: got %llu",
                (unsigned long long)b.data.u);
        }
    }

    /* ----------------------------------------------------------------
     * Macro Tests
     * ---------------------------------------------------------------- */

    TEST("DATABOX_LINEAR_DECODE macro") {
        databox a = {.data.i = -999, .type = DATABOX_SIGNED_64};
        databoxLinear dl = {0};
        ssize_t len = databoxLinearEncode(&a, &dl);

        databox b = {{0}};
        DATABOX_LINEAR_DECODE(&dl, &b, len + 1); /* +1 for type byte */

        if (b.data.i != -999) {
            ERR("Macro decode failed: got %lld", (long long)b.data.i);
        }
    }

    TEST("DATABOX_LINEAR_TYPE_IS_BYTES") {
        if (!DATABOX_LINEAR_TYPE_IS_BYTES(DATABOX_LINEAR_TYPE_BYTES)) {
            ERRR("TYPE_IS_BYTES should return true for BYTES type");
        }
        if (DATABOX_LINEAR_TYPE_IS_BYTES(0)) {
            ERRR("TYPE_IS_BYTES should return false for type 0");
        }
        if (DATABOX_LINEAR_TYPE_IS_BYTES(2)) {
            ERRR("TYPE_IS_BYTES should return false for type 2");
        }
    }

    /* ----------------------------------------------------------------
     * Encoding Size Tests
     * ---------------------------------------------------------------- */

    TEST("encoding sizes are minimal") {
        /* Values should encode to the smallest possible size */
        struct {
            uint64_t val;
            int expectedBytes;
        } cases[] = {
            {0, 1},          {255, 1},        {256, 2},
            {65535, 2},      {65536, 3},      {(1ULL << 24) - 1, 3},
            {1ULL << 24, 4}, {UINT32_MAX, 4}, {(uint64_t)UINT32_MAX + 1, 5},
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            databox a = {.data.u = cases[i].val, .type = DATABOX_UNSIGNED_64};
            databoxLinear dl = {0};
            ssize_t len = databoxLinearEncode(&a, &dl);

            if (len != cases[i].expectedBytes) {
                ERR("Value %llu should encode to %d bytes, got %zd",
                    (unsigned long long)cases[i].val, cases[i].expectedBytes,
                    len);
            }
        }
    }

    /* ================================================================
     * COMPREHENSIVE EDGE CASE TESTS
     * ================================================================ */

    TEST("unsigned - all byte boundary values") {
        /* Test every value at byte boundaries: 2^n-1, 2^n, 2^n+1 */
        for (int bits = 8; bits <= 64; bits += 8) {
            uint64_t boundary = (bits == 64) ? UINT64_MAX : (1ULL << bits);
            uint64_t vals[] = {boundary - 1, boundary, boundary + 1};
            int count = (bits == 64) ? 1 : 3; /* Can't go above UINT64_MAX */

            for (int i = 0; i < count; i++) {
                databox a = {.data.u = vals[i], .type = DATABOX_UNSIGNED_64};
                databoxLinear dl = {0};
                databoxLinearEncode(&a, &dl);

                databox b = {{0}};
                if (!databoxLinearDecode(&dl, &b)) {
                    ERR("Failed to decode at boundary 2^%d value %d", bits, i);
                }
                if (b.data.u != vals[i]) {
                    ERR("Boundary 2^%d[%d]: expected %llu, got %llu", bits, i,
                        (unsigned long long)vals[i],
                        (unsigned long long)b.data.u);
                }
            }
        }
    }

    TEST("signed - all byte boundary values") {
        /* Test signed values at byte boundaries */
        int64_t boundaries[] = {
            -128,          -129,          /* 1-byte boundary */
            -32768,        -32769,        /* 2-byte boundary */
            -8388608,      -8388609,      /* 3-byte boundary */
            -2147483648LL, -2147483649LL, /* 4-byte boundary */
            INT64_MIN,                    /* 8-byte boundary */
        };

        for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]);
             i++) {
            databox a = {.data.i = boundaries[i], .type = DATABOX_SIGNED_64};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            if (!databoxLinearDecode(&dl, &b)) {
                ERR("Failed to decode signed boundary %lld",
                    (long long)boundaries[i]);
            }
            if (b.data.i != boundaries[i]) {
                ERR("Signed boundary: expected %lld, got %lld",
                    (long long)boundaries[i], (long long)b.data.i);
            }
        }
    }

    TEST("signed - negative one encoding") {
        /* -1 is special: stored as 0 due to SIGNED_PREPARE optimization */
        databox a = {.data.i = -1, .type = DATABOX_SIGNED_64};
        databoxLinear dl = {0};
        ssize_t len = databoxLinearEncode(&a, &dl);

        if (len != 1) {
            ERR("-1 should encode to 1 byte, got %zd", len);
        }

        databox b = {{0}};
        databoxLinearDecode(&dl, &b);
        if (b.data.i != -1) {
            ERR("-1 roundtrip failed: got %lld", (long long)b.data.i);
        }
    }

    TEST("unsigned - powers of two") {
        for (int i = 0; i < 64; i++) {
            uint64_t val = 1ULL << i;
            databox a = {.data.u = val, .type = DATABOX_UNSIGNED_64};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);
            if (b.data.u != val) {
                ERR("2^%d roundtrip failed: expected %llu, got %llu", i,
                    (unsigned long long)val, (unsigned long long)b.data.u);
            }
        }
    }

    TEST("signed - negative powers of two") {
        for (int i = 0; i < 63; i++) {
            int64_t val = -(1LL << i);
            databox a = {.data.i = val, .type = DATABOX_SIGNED_64};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);
            if (b.data.i != val) {
                ERR("-2^%d roundtrip failed: expected %lld, got %lld", i,
                    (long long)val, (long long)b.data.i);
            }
        }
    }

    /* ================================================================
     * FLOAT SPECIAL VALUES
     * ================================================================ */

    TEST("float - special values") {
        float specials[10];
        specials[0] = 0.0f;
        specials[1] = -0.0f;
        specials[2] = 1.0f;
        specials[3] = -1.0f;
        specials[4] = FLT_MIN;
        specials[5] = FLT_MAX;
        specials[6] = -FLT_MIN;
        specials[7] = -FLT_MAX;
        specials[8] = FLT_EPSILON;
        specials[9] = -FLT_EPSILON;

        for (size_t i = 0; i < 10; i++) {
            databox a = {.data.f32 = specials[i], .type = DATABOX_FLOAT_32};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);

            if (b.data.f32 != specials[i]) {
                ERR("Float special[%zu] roundtrip failed: %g != %g", i,
                    (double)specials[i], (double)b.data.f32);
            }
        }
    }

    TEST("double - special values") {
        double specials[10];
        specials[0] = 0.0;
        specials[1] = -0.0;
        specials[2] = 1.0;
        specials[3] = -1.0;
        specials[4] = DBL_MIN;
        specials[5] = DBL_MAX;
        specials[6] = -DBL_MIN;
        specials[7] = -DBL_MAX;
        specials[8] = DBL_EPSILON;
        specials[9] = -DBL_EPSILON;

        for (size_t i = 0; i < 10; i++) {
            databox a = {.data.d64 = specials[i], .type = DATABOX_DOUBLE_64};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);

            /* Value may have been downgraded to float */
            double result =
                (b.type == DATABOX_DOUBLE_64) ? b.data.d64 : (double)b.data.f32;
            if (result != specials[i]) {
                ERR("Double special[%zu] roundtrip failed: %g != %g", i,
                    specials[i], result);
            }
        }
    }

    /* ================================================================
     * FUZZING TESTS - RANDOMIZED VALUES
     * ================================================================ */

    TEST("fuzz - 10000 random unsigned integers") {
        fuzzState = 0xDEADBEEFCAFEBABEULL; /* Reset seed for reproducibility */

        for (int i = 0; i < 10000; i++) {
            uint64_t val = fuzzNext();
            databox a = {.data.u = val, .type = DATABOX_UNSIGNED_64};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            if (!databoxLinearDecode(&dl, &b)) {
                ERR("Fuzz unsigned[%d]: decode failed for %llu", i,
                    (unsigned long long)val);
                break;
            }
            if (b.data.u != val) {
                ERR("Fuzz unsigned[%d]: expected %llu, got %llu", i,
                    (unsigned long long)val, (unsigned long long)b.data.u);
                break;
            }
        }
    }

    TEST("fuzz - 10000 random signed integers") {
        fuzzState = 0xFEEDFACE12345678ULL;

        for (int i = 0; i < 10000; i++) {
            int64_t val = fuzzSigned();
            databox a = {.data.i = val, .type = DATABOX_SIGNED_64};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            if (!databoxLinearDecode(&dl, &b)) {
                ERR("Fuzz signed[%d]: decode failed for %lld", i,
                    (long long)val);
                break;
            }

            /* Positive signed values decode as unsigned */
            int64_t result = (val >= 0) ? (int64_t)b.data.u : b.data.i;
            if (result != val) {
                ERR("Fuzz signed[%d]: expected %lld, got %lld", i,
                    (long long)val, (long long)result);
                break;
            }
        }
    }

    TEST("fuzz - 10000 random floats") {
        fuzzState = 0xABCDEF0123456789ULL;

        for (int i = 0; i < 10000; i++) {
            float val = fuzzFloat();
            databox a = {.data.f32 = val, .type = DATABOX_FLOAT_32};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            if (!databoxLinearDecode(&dl, &b)) {
                ERR("Fuzz float[%d]: decode failed", i);
                break;
            }
            if (b.data.f32 != val) {
                ERR("Fuzz float[%d]: expected %g, got %g", i, (double)val,
                    (double)b.data.f32);
                break;
            }
        }
    }

    TEST("fuzz - 10000 random doubles") {
        fuzzState = 0x9876543210FEDCBAULL;

        for (int i = 0; i < 10000; i++) {
            double val = fuzzDouble();
            databox a = {.data.d64 = val, .type = DATABOX_DOUBLE_64};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            if (!databoxLinearDecode(&dl, &b)) {
                ERR("Fuzz double[%d]: decode failed", i);
                break;
            }

            double result =
                (b.type == DATABOX_DOUBLE_64) ? b.data.d64 : (double)b.data.f32;
            if (result != val) {
                ERR("Fuzz double[%d]: expected %g, got %g", i, val, result);
                break;
            }
        }
    }

    TEST("fuzz - small unsigned values (1-3 bytes)") {
        fuzzState = 0x1111111111111111ULL;

        for (int i = 0; i < 10000; i++) {
            /* Values 0 to 2^24-1 (1-3 bytes) */
            uint64_t val = fuzzRange((1ULL << 24) - 1);
            databox a = {.data.u = val, .type = DATABOX_UNSIGNED_64};
            databoxLinear dl = {0};
            ssize_t len = databoxLinearEncode(&a, &dl);

            /* Verify minimal encoding */
            int expectedLen = (val <= 255) ? 1 : (val <= 65535) ? 2 : 3;
            if (len != expectedLen) {
                ERR("Fuzz small[%d]: %llu should be %d bytes, got %zd", i,
                    (unsigned long long)val, expectedLen, len);
            }

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);
            if (b.data.u != val) {
                ERR("Fuzz small[%d]: roundtrip failed", i);
                break;
            }
        }
    }

    TEST("fuzz - small negative values (-1 to -2^23)") {
        fuzzState = 0x2222222222222222ULL;

        for (int i = 0; i < 10000; i++) {
            int64_t val = -1 - (int64_t)fuzzRange((1ULL << 23) - 1);
            databox a = {.data.i = val, .type = DATABOX_SIGNED_64};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);
            if (b.data.i != val) {
                ERR("Fuzz neg[%d]: expected %lld, got %lld", i, (long long)val,
                    (long long)b.data.i);
                break;
            }
        }
    }

    /* ================================================================
     * ROUND-TRIP CONSISTENCY TESTS
     * ================================================================ */

    TEST("roundtrip - encode twice gives same result") {
        uint64_t vals[] = {0, 1, 255, 256, 65535, UINT32_MAX, UINT64_MAX};

        for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
            databox a = {.data.u = vals[i], .type = DATABOX_UNSIGNED_64};

            databoxLinear dl1 = {0}, dl2 = {0};
            ssize_t len1 = databoxLinearEncode(&a, &dl1);
            ssize_t len2 = databoxLinearEncode(&a, &dl2);

            if (len1 != len2) {
                ERR("Double encode length mismatch for %llu",
                    (unsigned long long)vals[i]);
            }
            if (dl1.type != dl2.type) {
                ERR("Double encode type mismatch for %llu",
                    (unsigned long long)vals[i]);
            }
            if (memcmp(&dl1.data, &dl2.data, len1) != 0) {
                ERR("Double encode data mismatch for %llu",
                    (unsigned long long)vals[i]);
            }
        }
    }

    TEST("roundtrip - decode twice gives same result") {
        databox a = {.data.u = 123456789, .type = DATABOX_UNSIGNED_64};
        databoxLinear dl = {0};
        databoxLinearEncode(&a, &dl);

        databox b1 = {{0}}, b2 = {{0}};
        databoxLinearDecode(&dl, &b1);
        databoxLinearDecode(&dl, &b2);

        if (b1.type != b2.type || b1.data.u != b2.data.u) {
            ERRR("Double decode gave different results");
        }
    }

    TEST("roundtrip - encode/decode/encode cycle") {
        fuzzState = 0x3333333333333333ULL;

        for (int i = 0; i < 1000; i++) {
            uint64_t val = fuzzNext();
            databox a = {.data.u = val, .type = DATABOX_UNSIGNED_64};

            /* Encode */
            databoxLinear dl1 = {0};
            ssize_t len1 = databoxLinearEncode(&a, &dl1);

            /* Decode */
            databox b = {{0}};
            databoxLinearDecode(&dl1, &b);

            /* Re-encode */
            databoxLinear dl2 = {0};
            ssize_t len2 = databoxLinearEncode(&b, &dl2);

            /* Should produce identical encoding */
            if (len1 != len2 || dl1.type != dl2.type ||
                memcmp(&dl1.data, &dl2.data, len1) != 0) {
                ERR("Encode/decode/encode cycle failed for %llu",
                    (unsigned long long)val);
                break;
            }
        }
    }

    /* ================================================================
     * PARTS API TESTS
     * ================================================================ */

    TEST("DecodeParts - all types") {
        /* Test DecodeParts with various types */
        databox inputs[] = {
            {.data.u = 42, .type = DATABOX_UNSIGNED_64},
            {.data.i = -42, .type = DATABOX_SIGNED_64},
            {.data.f32 = 3.14f, .type = DATABOX_FLOAT_32},
            {.data.d64 = 2.718281828, .type = DATABOX_DOUBLE_64},
            {.type = DATABOX_TRUE},
            {.type = DATABOX_FALSE},
            {.type = DATABOX_NULL},
        };

        for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
            databoxLinear dl = {0};
            databoxLinearEncode(&inputs[i], &dl);

            databox b = {{0}};
            bool ok = databoxLinearDecodeParts(dl.type, &dl.data, &b);
            if (!ok) {
                ERR("DecodeParts failed for input[%zu]", i);
            }

            /* Verify type (accounting for signed->unsigned promotion) */
            if (inputs[i].type == DATABOX_SIGNED_64 && inputs[i].data.i >= 0) {
                /* Positive signed decodes as unsigned - OK */
            } else if (b.type != inputs[i].type &&
                       !(inputs[i].type == DATABOX_DOUBLE_64 &&
                         b.type == DATABOX_FLOAT_32)) {
                ERR("DecodeParts type mismatch for input[%zu]: %d vs %d", i,
                    (int)inputs[i].type, (int)b.type);
            }
        }
    }

    TEST("PARTS_DECODE macro - fixed types") {
        databox a = {.data.u = 999, .type = DATABOX_UNSIGNED_64};
        databoxLinear dl = {0};
        ssize_t len = databoxLinearEncode(&a, &dl);

        databox b = {{0}};
        DATABOX_LINEAR_PARTS_DECODE(dl.type, (const uint8_t *)&dl.data, len,
                                    &b);

        if (b.data.u != 999) {
            ERR("PARTS_DECODE macro failed: got %llu",
                (unsigned long long)b.data.u);
        }
    }

    /* ================================================================
     * ENCODING WIDTH VERIFICATION
     * ================================================================ */

    TEST("encoding width - unsigned exhaustive") {
        /* Verify every unsigned value encodes to expected width */
        struct {
            uint64_t maxVal;
            int width;
        } ranges[] = {
            {255, 1},
            {65535, 2},
            {(1ULL << 24) - 1, 3},
            {UINT32_MAX, 4},
            {(1ULL << 40) - 1, 5},
            {(1ULL << 48) - 1, 6},
            {(1ULL << 56) - 1, 7},
            {UINT64_MAX, 8},
        };

        for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
            /* Test max value for this width */
            databox a = {.data.u = ranges[i].maxVal,
                         .type = DATABOX_UNSIGNED_64};
            databoxLinear dl = {0};
            ssize_t len = databoxLinearEncode(&a, &dl);

            if (len != ranges[i].width) {
                ERR("Max value %llu should encode to %d bytes, got %zd",
                    (unsigned long long)ranges[i].maxVal, ranges[i].width, len);
            }

            /* Also test min value for this width (if not first range) */
            if (i > 0) {
                uint64_t minVal = ranges[i - 1].maxVal + 1;
                a.data.u = minVal;
                len = databoxLinearEncode(&a, &dl);
                if (len != ranges[i].width) {
                    ERR("Min value %llu should encode to %d bytes, got %zd",
                        (unsigned long long)minVal, ranges[i].width, len);
                }
            }
        }
    }

    TEST("encoding width - signed exhaustive") {
        /* Verify signed values encode to expected width */
        struct {
            int64_t minVal;
            int width;
        } ranges[] = {
            {-128, 1},          {-32768, 2},       {-8388608, 3},
            {-2147483648LL, 4}, {-(1LL << 39), 5}, {-(1LL << 47), 6},
            {-(1LL << 55), 7},  {INT64_MIN, 8},
        };

        for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
            databox a = {.data.i = ranges[i].minVal, .type = DATABOX_SIGNED_64};
            databoxLinear dl = {0};
            ssize_t len = databoxLinearEncode(&a, &dl);

            if (len != ranges[i].width) {
                ERR("Signed %lld should encode to %d bytes, got %zd",
                    (long long)ranges[i].minVal, ranges[i].width, len);
            }
        }
    }

    /* ================================================================
     * SEQUENTIAL VALUE TESTS
     * ================================================================ */

    TEST("sequential unsigned 0-1000") {
        for (uint64_t val = 0; val <= 1000; val++) {
            databox a = {.data.u = val, .type = DATABOX_UNSIGNED_64};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);
            if (b.data.u != val) {
                ERR("Sequential %llu failed", (unsigned long long)val);
                break;
            }
        }
    }

    TEST("sequential signed -500 to +500") {
        for (int64_t val = -500; val <= 500; val++) {
            databox a = {.data.i = val, .type = DATABOX_SIGNED_64};
            databoxLinear dl = {0};
            databoxLinearEncode(&a, &dl);

            databox b = {{0}};
            databoxLinearDecode(&dl, &b);

            int64_t result = (val >= 0) ? (int64_t)b.data.u : b.data.i;
            if (result != val) {
                ERR("Sequential signed %lld failed", (long long)val);
                break;
            }
        }
    }

    TEST_FINAL_RESULT;
}
#endif
