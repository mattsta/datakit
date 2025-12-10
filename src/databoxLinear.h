#pragma once

/* ============================================================================
 * databoxLinear - Compact Binary Serialization for databox Values
 * ============================================================================
 * Provides space-efficient encoding for on-disk/network storage:
 *   - 1-byte type tag + minimal value bytes
 *   - Integers stored in smallest possible width (1-8 bytes)
 *   - Floats auto-downgrade to float16 if lossless
 *   - Doubles auto-downgrade to float32/float16 if lossless
 *   - Boolean/null types are just the type byte (0 value bytes)
 *
 * IMPORTANT: This is a stable on-disk format. Type IDs must never change.
 *
 * Two APIs:
 *   1. Functions: For fixed-width types (integers, floats, booleans)
 *   2. Macros: For BYTES type which needs external length
 *
 * Usage for fixed types:
 *   databox box = DATABOX_UNSIGNED(42);
 *   databoxLinear linear;
 *   ssize_t len = databoxLinearEncode(&box, &linear);
 *   // linear.type + linear.data[0..len-1] is the encoding
 *
 *   databox decoded;
 *   databoxLinearDecode(&linear, &decoded);
 *
 * Usage for BYTES type (use macros due to external length):
 *   // Encoding: use DATABOX_LINEAR_PARTS_ENCODE
 *   // Decoding: use DATABOX_LINEAR_DECODE
 * ============================================================================
 */

#include "databox.h"

/* Packed structure for linear encoding: type byte + value union */
typedef struct __attribute__((packed)) databoxLinear {
    uint8_t type;
    databoxUnion data;
} databoxLinear;

/* ----------------------------------------------------------------------------
 * Type Detection (for BYTES special handling)
 * --------------------------------------------------------------------------*/
/* BYTES type ID is 1 (stable, will not change) */
#define DATABOX_LINEAR_TYPE_BYTES 1

#define DATABOX_LINEAR_TYPE_IS_BYTES(type) ((type) == DATABOX_LINEAR_TYPE_BYTES)
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

/* Decode linear box when stored as broken up type/value/length details.
 * Parameters:
 *   type_:       the type byte
 *   val:         pointer to value bytes
 *   valueLength: length of value bytes (used for BYTES type)
 *   box:         output databox to populate */
#define DATABOX_LINEAR_PARTS_DECODE(type_, val, valueLength, box)              \
    do {                                                                       \
        if (DATABOX_LINEAR_TYPE_IS_BYTES(type_)) {                             \
            /* Point box data to data inside the linear box */                 \
            (box)->data.bytes.custart = (val);                                 \
            (box)->type = DATABOX_BYTES;                                       \
            (box)->len = (valueLength);                                        \
        } else {                                                               \
            /* Fixed-width type: decode based on type */                       \
            databoxLinearDecodeParts(type_, val, box);                         \
        }                                                                      \
    } while (0)

/* Encode a databox to linear format, populating output variables.
 * Parameters:
 *   box:           input databox to encode
 *   encodedLength: output variable for encoded value length
 *   encodedValue:  output variable for pointer to encoded value
 *   encodedType:   output variable for type byte
 *   boxl:          scratch databoxLinear for non-BYTES types */
#define DATABOX_LINEAR_PARTS_ENCODE(box, encodedLength, encodedValue,          \
                                    encodedType, boxl)                         \
    do {                                                                       \
        if (DATABOX_IS_BYTES(box)) {                                           \
            encodedValue = databoxBytes(box);                                  \
            encodedLength = databoxLen(box);                                   \
            encodedType = DATABOX_LINEAR_TYPE_BYTES;                           \
        } else {                                                               \
            encodedLength = databoxLinearEncode(box, boxl);                    \
            encodedValue = &(boxl)->data;                                      \
            encodedType = (boxl)->type;                                        \
        }                                                                      \
    } while (0)

/* ----------------------------------------------------------------------------
 * Core Functions (for fixed-width types, NOT BYTES)
 * --------------------------------------------------------------------------*/

/* Encode databox to linear format.
 * NOTE: Does NOT handle DATABOX_BYTES - use DATABOX_LINEAR_PARTS_ENCODE macro.
 * Returns: length of encoded value (0 for bool/null, 1-8 for integers, etc.)
 *          or -1 on error (unsupported type) */
ssize_t databoxLinearEncode(const databox *restrict box,
                            databoxLinear *restrict boxl);

/* Decode linear format back to databox.
 * NOTE: Does NOT handle BYTES - use DATABOX_LINEAR_DECODE macro.
 * Returns: true on success, false on invalid type */
bool databoxLinearDecode(const databoxLinear *restrict boxl,
                         databox *restrict box);

/* Decode from separate type and value pointer (for when not using packed
 * struct). Returns: true on success, false on invalid type */
bool databoxLinearDecodeParts(const uint_fast8_t type,
                              const void *restrict start,
                              databox *restrict box);

#ifdef DATAKIT_TEST
int databoxLinearTest(int argc, char *argv[]);
#endif
