#pragma once

/* clusterMailbox internals: the inner-frame header codec Seal and Ingest
 * use, exposed to the mailbox's own tests */

#include "clusterMailbox.h"

#include <stddef.h>
#include <stdint.h>

/* Worst-case bytes the inner-header flex can occupy.  The buffer given to
 * clusterMailboxEncodeInnerHeader MUST be at least this large. */
#define CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES ((size_t)48)

/* The inner header is a 5-entry flex of unsigned values, ahead of the
 * opaque payload:
 *   protocol version | protocol features | origin_id (u32) |
 *   sequence (u64) | msg_type (u8) */

/* Encode an inner-frame header (stamped with this build's
 * clusterMailboxProtocol) into `out` (cap >=
 * CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES). Returns the encoded byte length
 * on success or 0 on bad args / insufficient capacity / allocation
 * failure. */
size_t clusterMailboxEncodeInnerHeader(uint8_t *out, size_t out_cap,
                                       uint32_t origin_id, uint64_t sequence,
                                       uint8_t msg_type);

/* Decode an inner-frame header from `src` (length `src_len`, which may
 * include trailing opaque payload bytes after the flex prefix).
 *
 * On success returns true; sets `*protocol` to the frame's protocol stamp
 * (the caller checks it against clusterMailboxProtocol), `*origin_id`,
 * `*sequence`, `*msg_type` to the decoded values and `*hdr_len` to the
 * byte length the flex prefix consumed (caller computes payload region as
 * `src + *hdr_len`, `src_len - *hdr_len`).
 *
 * Returns false on any of: short buffer, malformed flex framing, wrong
 * entry count, an entry that is not a 64-bit unsigned integer
 * (96/128-bit included), or any value outside its representable range
 * (protocol version or origin_id > UINT32_MAX, msg_type > UINT8_MAX).
 *
 * Safe under arbitrary input: the framing check uses
 * flexValidateBufferDeep, which walks every entry's length-encoding bytes
 * before any iterator runs, so even bytes that never passed an envelope
 * check cannot make flexNext / flexGetByTypeBig read past the buffer. */
bool clusterMailboxDecodeInnerHeader(const uint8_t *src, size_t src_len,
                                     dkFormatStamp *protocol,
                                     uint32_t *origin_id, uint64_t *sequence,
                                     uint8_t *msg_type, size_t *hdr_len);
