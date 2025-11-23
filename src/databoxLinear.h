#pragma once

#include "databox.h"

typedef struct __attribute__((packed)) databoxLinear {
    uint8_t type;
    databoxUnion data;
} databoxLinear;

/* This is some abstraction leakage with an explicit number (because
 * the type enum is private inside 'databoxLinear.c'), but the types are fixed
 * and order won't be modified. */
#define DATABOX_LINEAR_TYPE_IS_BYTES(type) ((type) == 1)
#define DATABOX_LINEAR_IS_BYTES(dl) DATABOX_LINEAR_TYPE_IS_BYTES((dl)->type)

/* We use a macro for total decode because decoding BYTES requires a length,
 * while decoding the fixed types don't require the box length since we know
 * their widths from the encoding. */
#define DATABOX_LINEAR_DECODE(dl, box, linearLength)                           \
    do {                                                                       \
        if (DATABOX_LINEAR_IS_BYTES(dl)) {                                     \
            /* Point box data to data inside the linear box */                 \
            (box)->data.bytes.custart = (dl)->data.bytes.custart;              \
            (box)->type = DATABOX_BYTES;                                       \
                                                                               \
            /* Drop the type byte from total length for BYTES length */        \
            (box)->len = (linearLength) - 1;                                   \
        } else {                                                               \
            /* else, data is a known-length based on the type */               \
            databoxLinearDecode(dl, box);                                      \
        }                                                                      \
    } while (0)

/* Decode linear box when stored as broken up type/value/length details */
#define DATABOX_LINEAR_PARTS_DECODE(type_, val, valueLength, box)              \
    do {                                                                       \
        if (DATABOX_LINEAR_TYPE_IS_BYTES(type_)) {                             \
            /* Point box data to data inside the linear box */                 \
            (box)->data.bytes.custart = (val);                                 \
            (box)->type = DATABOX_BYTES;                                       \
                                                                               \
            /* Drop the type byte from total length for BYTES length */        \
            (box)->len = (valueLength);                                        \
        } else {                                                               \
            /* else, data is a known-length based on the type */               \
            databoxLinearDecodeParts(type_, value, box);                       \
        }                                                                      \
    } while (0)

#define DATABOX_LINEAR_PARTS_ENCODE(box, encodedLength, encodedValue,          \
                                    encodedType, boxl)                         \
    do {                                                                       \
        if (DATABOX_IS_BYTES(box)) {                                           \
            encodedValue = databoxBytes(box);                                  \
            encodedLength = databoxLen(box);                                   \
            encodedType = 1; /* boo abstraction breakage */                    \
        } else {                                                               \
            encodedLength = databoxLinearEncode(box, boxl);                    \
            encodedValue = &(boxl)->data;                                      \
            encodedType = (boxl)->type;                                        \
        }                                                                      \
    } while (0)

/* Returns length of linear encoding */
ssize_t databoxLinearEncode(const databox *restrict box,
                            databoxLinear *restrict boxl);

/* Populates box on success */
bool databoxLinearDecode(const databoxLinear *restrict boxl,
                         databox *restrict box);

/* Decode a box given already broken apart type/value data */
bool databoxLinearDecodeParts(const uint_fast8_t type,
                              const void *restrict start,
                              databox *restrict box);

#ifdef DATAKIT_TEST
int databoxLinearTest(int argc, char *argv[]);
#endif
