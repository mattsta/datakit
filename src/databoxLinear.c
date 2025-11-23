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
        assert(NULL); /* unsupported encoding */
        /* TODO: better error handling */
        return -1;
        __builtin_unreachable();
    }

    __builtin_unreachable();
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
        /* Incorrect encoding. Error restoring. */
        assert(NULL);
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
#include "stdio.h"

bool encodeDecodeTestUnsigned(uintmax_t u) {
    int err = 0;

    databox a = {.data.u = u, .type = DATABOX_UNSIGNED_64};
    databoxLinear dbl = {0};
    databoxLinearEncode(&a, &dbl);

    databox b = {{0}};
    databoxLinearDecode(&dbl, &b);

    if (b.type != DATABOX_UNSIGNED_64) {
        ERRR("Type not restored!");
    }

    if (b.data.u != u) {
        ERRR("Value not restored!");
    }

    if (err) {
        return false;
    }

    return true;
}

bool encodeDecodeTestSigned(intmax_t u) {
    int err = 0;

    databox a = {.data.i = u, .type = DATABOX_SIGNED_64};
    databoxLinear dbl = {0};
    databoxLinearEncode(&a, &dbl);

    databox b = {{0}};
    databoxLinearDecode(&dbl, &b);

    if (u < 0 && (b.type != DATABOX_SIGNED_64)) {
        ERRR("Type not restored!");
    }

    if (b.data.i != u) {
        ERR("Value not restored! Expected %" PRIdMAX " but got %" PRIdMAX, u,
            b.data.i);
    }

    if (err) {
        return false;
    }

    return true;
}

bool encodeDecodeTestType(databoxType type) {
    int err = 0;

    databox a = {.type = type};
    databoxLinear dbl = {0};
    databoxLinearEncode(&a, &dbl);

    databox b = {{0}};
    databoxLinearDecode(&dbl, &b);

    if (b.type != type) {
        ERRR("Type not restored!");
    }

    if (err) {
        return false;
    }

    return true;
}

bool encodeDecodeTestFloat(float u) {
    int err = 0;

    for (int i = 0; i < 500000; i++) {
        databox a = {.data.f32 = u * 2.7 * i, .type = DATABOX_FLOAT_32};
        databoxLinear dbl = {0};
        const bool encoded = databoxLinearEncode(&a, &dbl);
        assert(encoded);

        databox b = {{0}};
        const bool worked = databoxLinearDecode(&dbl, &b);
        assert(worked);

        if (b.type != DATABOX_FLOAT_32) {
            assert(NULL);
            ERRR("Type not restored!");
        }

        if (b.data.f32 != a.data.f32) {
            ERR("Value not restored! Expected %g but got %g", a.data.f32,
                b.data.f32);
        }

        if (err) {
            return false;
        }
    }

    return true;
}

bool encodeDecodeTestDouble(double u) {
    int err = 0;

    for (int i = 0; i < 500000; i++) {
        databox a = {.data.d64 = u * 2.7 * i, .type = DATABOX_DOUBLE_64};
        databoxLinear dbl = {0};
        const bool encoded = databoxLinearEncode(&a, &dbl);
        assert(encoded);

        databox b = {{0}};
        const bool worked = databoxLinearDecode(&dbl, &b);
        assert(worked);

        /* If double got converted to float with no loss,
         * check for float type */
        if ((double)(float)(u * 2.7 * i) == (u * 2.7 * i)) {
            if (b.type != DATABOX_FLOAT_32) {
                ERRR("Type not restored!");
            }
            if (b.data.f32 != u * 2.7 * i) {
                assert(NULL);
                ERR("Value not restored! Expected %g but got %g", u * 2.7 * i,
                    b.data.f32);
            }
        } else {
            /* else, we remained a double after conversion */
            if (b.type != DATABOX_DOUBLE_64) {
                ERRR("Type not restored!");
            }

            if (b.data.d64 != u * 2.7 * i) {
                ERR("Value not restored! Expected %g but got %g", u * 2.7 * i,
                    b.data.d64);
            }
        }

        if (err) {
            return false;
        }
    }

    return true;
}

int databoxLinearTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    TEST("Encode unsigned integers...") {
        encodeDecodeTestUnsigned(0);
        encodeDecodeTestUnsigned(1);
        encodeDecodeTestUnsigned(2);
        encodeDecodeTestUnsigned(3);
        for (size_t i = 0; i < 64; i++) {
            encodeDecodeTestUnsigned(1ULL << i);
        }
    }

    TEST("Encode negative integers...") {
        encodeDecodeTestSigned(-0);
        encodeDecodeTestSigned(-1);
        encodeDecodeTestSigned(-2);
        encodeDecodeTestSigned(-3);
        for (size_t i = 0; i < 62; i++) {
            encodeDecodeTestSigned(-1ULL << i);
        }
    }

    TEST("Encode floats...") {
        encodeDecodeTestFloat(0);
        encodeDecodeTestFloat(1);
        encodeDecodeTestFloat(2);
        encodeDecodeTestFloat(3);
        encodeDecodeTestFloat(4);
        encodeDecodeTestFloat(INT_MAX);
    }

    TEST("Encode doubles...") {
        encodeDecodeTestDouble(0);
        encodeDecodeTestDouble(1);
        encodeDecodeTestDouble(2);
        encodeDecodeTestDouble(3);
        encodeDecodeTestDouble(4);
        encodeDecodeTestDouble(INT_MAX);
    }

    TEST("Encode true, false, null...") {
        encodeDecodeTestType(DATABOX_NULL);
        encodeDecodeTestType(DATABOX_TRUE);
        encodeDecodeTestType(DATABOX_FALSE);
    }

    /* The result here lies because we're not accumulating 'err'
     * growth inside the test function; all errors are reported
     * in the helper functions and not returning their error
     * count back here. */
    TEST_FINAL_RESULT;
}
#endif
