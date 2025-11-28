/**
 * network_protocol.c - Custom network protocol using multiple varint types
 *
 * This example demonstrates a network protocol combining:
 * - varintBitstream: Bit-packed protocol headers
 * - varintChained: Protocol Buffers compatibility
 * - varintExternal: Payload data
 *
 * Features:
 * - Space-efficient packet headers
 * - Protocol Buffers message encoding
 * - Mixed-width field packing
 * - Message framing
 *
 * Compile: gcc -I../src network_protocol.c ../build/src/libvarint.a -o
 * network_protocol Run: ./network_protocol
 */

#include "varintBitstream.h"
#include "varintChained.h"
#include "varintExternal.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// CUSTOM PROTOCOL HEADER (using varintBitstream)
// ============================================================================

/*
 * Header format (28 bits total):
 * - Version: 3 bits (0-7)
 * - Type: 5 bits (0-31)
 * - Flags: 8 bits
 * - Length: 12 bits (0-4095)
 */

typedef struct {
    uint8_t version; // 3 bits
    uint8_t type;    // 5 bits
    uint8_t flags;   // 8 bits
    uint16_t length; // 12 bits
} PacketHeader;

#define HEADER_BYTES 4 // 28 bits = 4 bytes

void encodeHeader(uint8_t *buffer, const PacketHeader *header) {
    uint64_t packed = 0;
    size_t offset = 0;

    // Pack all fields into single uint64
    varintBitstreamSet(&packed, offset, 3, header->version);
    offset += 3;

    varintBitstreamSet(&packed, offset, 5, header->type);
    offset += 5;

    varintBitstreamSet(&packed, offset, 8, header->flags);
    offset += 8;

    varintBitstreamSet(&packed, offset, 12, header->length);
    (void)offset; // Last use of offset

    // varintBitstream packs from high bits down, so shift right to move data to
    // low bits We used 28 bits total, so shift right by (64 - 28) = 36 bits
    packed >>= (64 - 28);

    // Write to buffer (native endianness via memcpy)
    memcpy(buffer, &packed, HEADER_BYTES);
}

void decodeHeader(const uint8_t *buffer, PacketHeader *header) {
    // Read from buffer (native endianness via memcpy)
    uint64_t packed = 0;
    memcpy(&packed, buffer, HEADER_BYTES);

    // varintBitstream expects data in high bits, so shift left
    // We have 28 bits of data, so shift left by (64 - 28) = 36 bits
    packed <<= (64 - 28);

    size_t offset = 0;

    header->version = varintBitstreamGet(&packed, offset, 3);
    offset += 3;

    header->type = varintBitstreamGet(&packed, offset, 5);
    offset += 5;

    header->flags = varintBitstreamGet(&packed, offset, 8);
    offset += 8;

    header->length = varintBitstreamGet(&packed, offset, 12);
}

// ============================================================================
// PROTOCOL BUFFERS MESSAGE (using varintChained)
// ============================================================================

/*
 * Simple Protocol Buffers message:
 * message UserInfo {
 *   uint64 user_id = 1;
 *   uint32 age = 2;
 *   string name = 3;
 * }
 */

typedef struct {
    uint64_t userId;
    uint32_t age;
    char name[32];
} UserInfo;

// Protobuf wire types
#define WIRE_TYPE_VARINT 0
#define WIRE_TYPE_LENGTH_DELIMITED 2

void encodeProtobuf(uint8_t *buffer, size_t *offset, const UserInfo *user) {
    // Field 1: user_id (varint)
    uint64_t tag1 = (1 << 3) | WIRE_TYPE_VARINT;
    *offset += varintChainedPutVarint(buffer + *offset, tag1);
    *offset += varintChainedPutVarint(buffer + *offset, user->userId);

    // Field 2: age (varint)
    uint64_t tag2 = (2 << 3) | WIRE_TYPE_VARINT;
    *offset += varintChainedPutVarint(buffer + *offset, tag2);
    *offset += varintChainedPutVarint(buffer + *offset, user->age);

    // Field 3: name (length-delimited string)
    uint64_t tag3 = (3 << 3) | WIRE_TYPE_LENGTH_DELIMITED;
    *offset += varintChainedPutVarint(buffer + *offset, tag3);

    size_t nameLen = strlen(user->name);
    *offset += varintChainedPutVarint(buffer + *offset, nameLen);
    memcpy(buffer + *offset, user->name, nameLen);
    *offset += nameLen;
}

void decodeProtobuf(const uint8_t *buffer, size_t *offset, UserInfo *user) {
    memset(user, 0, sizeof(UserInfo));

    while (1) {
        // Read tag
        uint64_t tag;
        size_t tagLen = varintChainedGetVarint(buffer + *offset, &tag);
        if (tagLen == 0) {
            break;
        }
        *offset += tagLen;

        uint32_t fieldNumber = tag >> 3;
        uint32_t wireType = tag & 0x07;

        switch (fieldNumber) {
        case 1: // user_id
            assert(wireType == WIRE_TYPE_VARINT);
            *offset += varintChainedGetVarint(buffer + *offset, &user->userId);
            break;

        case 2: { // age
            assert(wireType == WIRE_TYPE_VARINT);
            uint64_t age64;
            *offset += varintChainedGetVarint(buffer + *offset, &age64);
            user->age = (uint32_t)age64;
            break;
        }

        case 3: { // name
            assert(wireType == WIRE_TYPE_LENGTH_DELIMITED);
            uint64_t nameLen;
            *offset += varintChainedGetVarint(buffer + *offset, &nameLen);
            memcpy(user->name, buffer + *offset, nameLen);
            user->name[nameLen] = '\0';
            *offset += nameLen;
            return; // End of message
        }

        default:
            printf("Unknown field: %u\n", fieldNumber);
            return;
        }
    }
}

// ============================================================================
// COMPLETE PACKET (Header + Payload)
// ============================================================================

typedef struct {
    PacketHeader header;
    UserInfo payload;
} Packet;

size_t encodePacket(uint8_t *buffer, const Packet *packet) {
    size_t offset = 0;

    // Encode payload first (to get length)
    uint8_t payloadBuffer[256];
    size_t payloadLen = 0;
    encodeProtobuf(payloadBuffer, &payloadLen, &packet->payload);

    // Set header length
    PacketHeader header = packet->header;
    header.length = payloadLen;

    // Encode header
    encodeHeader(buffer, &header);
    offset += HEADER_BYTES;

    // Copy payload
    memcpy(buffer + offset, payloadBuffer, payloadLen);
    offset += payloadLen;

    return offset;
}

void decodePacket(const uint8_t *buffer, Packet *packet) {
    size_t offset = 0;

    // Decode header
    decodeHeader(buffer, &packet->header);
    offset += HEADER_BYTES;

    // Decode payload (length from header)
    decodeProtobuf(buffer, &offset, &packet->payload);
}

// ============================================================================
// MESSAGE FRAMING (varintExternal for length prefix)
// ============================================================================

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t used;
} MessageStream;

void streamInit(MessageStream *stream, size_t capacity) {
    stream->buffer = malloc(capacity);
    stream->capacity = capacity;
    stream->used = 0;
}

void streamAppend(MessageStream *stream, const uint8_t *data, size_t len) {
    assert(stream->used + 1 + len <= stream->capacity);

    // Write length prefix (varintExternal)
    varintWidth lengthWidth =
        varintExternalPut(stream->buffer + stream->used, len);
    stream->used += lengthWidth;

    // Write data
    memcpy(stream->buffer + stream->used, data, len);
    stream->used += len;
}

bool streamRead(MessageStream *stream, size_t *offset, uint8_t *data,
                size_t *len) {
    if (*offset >= stream->used) {
        return false;
    }

    // Read length prefix
    varintWidth lengthWidth;
    varintExternalUnsignedEncoding(stream->buffer[*offset], lengthWidth);
    uint64_t messageLen =
        varintExternalGet(stream->buffer + *offset, lengthWidth);
    *offset += lengthWidth;

    // Read data
    memcpy(data, stream->buffer + *offset, messageLen);
    *len = messageLen;
    *offset += messageLen;

    return true;
}

void streamFree(MessageStream *stream) {
    free(stream->buffer);
}

// ============================================================================
// DEMONSTRATION
// ============================================================================

void demonstrateProtocol(void) {
    printf("\n=== Network Protocol Example ===\n\n");

    // 1. Create and encode packet
    printf("1. Creating packet with custom header...\n");

    Packet packet = {
        .header = {.version = 2, .type = 5, .flags = 0x42, .length = 0},
        .payload = {.userId = 123456, .age = 28, .name = "Alice"}};

    printf("   Version: %u\n", packet.header.version);
    printf("   Type: %u\n", packet.header.type);
    printf("   Flags: 0x%02X\n", packet.header.flags);
    printf("   Payload: UserID=%" PRIu64 ", Age=%u, Name=%s\n",
           packet.payload.userId, packet.payload.age, packet.payload.name);

    uint8_t packetBuffer[512];
    size_t packetLen = encodePacket(packetBuffer, &packet);

    printf("   Encoded packet: %zu bytes\n", packetLen);
    printf("   Header: 4 bytes (28 bits packed)\n");
    printf("   Payload: %zu bytes (Protocol Buffers)\n", packetLen - 4);

    // 2. Decode packet
    printf("\n2. Decoding packet...\n");

    Packet decoded;
    decodePacket(packetBuffer, &decoded);

    printf("   Version: %u\n", decoded.header.version);
    printf("   Type: %u\n", decoded.header.type);
    printf("   Flags: 0x%02X\n", decoded.header.flags);
    printf("   Length: %u\n", decoded.header.length);
    printf("   Payload: UserID=%" PRIu64 ", Age=%u, Name=%s\n",
           decoded.payload.userId, decoded.payload.age, decoded.payload.name);

    assert(decoded.header.version == packet.header.version);
    assert(decoded.header.type == packet.header.type);
    assert(decoded.header.flags == packet.header.flags);
    assert(decoded.payload.userId == packet.payload.userId);
    assert(decoded.payload.age == packet.payload.age);
    assert(strcmp(decoded.payload.name, packet.payload.name) == 0);

    printf("   ✓ Packet decoded correctly\n");

    // 3. Message framing with stream
    printf("\n3. Creating message stream with multiple packets...\n");

    MessageStream stream;
    streamInit(&stream, 2048);

    // Create and encode 3 different packets
    Packet packets[3] = {
        {{1, 3, 0x01, 0}, {111, 25, "Bob"}},
        {{1, 3, 0x02, 0}, {222, 30, "Carol"}},
        {{1, 3, 0x03, 0}, {333, 35, "Dave"}},
    };

    for (int i = 0; i < 3; i++) {
        size_t len = encodePacket(packetBuffer, &packets[i]);
        streamAppend(&stream, packetBuffer, len);
        printf("   Appended packet %d (%zu bytes)\n", i + 1, len);
    }

    printf("   Stream contains %zu bytes\n", stream.used);

    // 4. Read and decode from stream
    printf("\n4. Reading packets from stream...\n");

    size_t offset = 0;
    int count = 0;
    uint8_t messageBuffer[256];
    size_t messageLen;

    while (streamRead(&stream, &offset, messageBuffer, &messageLen)) {
        Packet pkt;
        decodePacket(messageBuffer, &pkt);
        printf("   Packet %d: UserID=%" PRIu64 ", Name=%s\n", ++count,
               pkt.payload.userId, pkt.payload.name);
    }

    assert(count == 3);
    printf("   ✓ Read %d packets from stream\n", count);

    // 5. Space efficiency analysis
    printf("\n5. Space efficiency analysis:\n");

    // Header comparison
    printf("   Custom header (varintBitstream): 4 bytes (28 bits)\n");
    printf("   Fixed header (4 separate bytes):  4 bytes (32 bits)\n");
    printf("   Savings: 4 bits per packet (12.5%%)\n");

    // Protobuf comparison
    printf("\n   Protocol Buffers (varintChained):\n");
    printf("   - UserID 123456: %d bytes (vs 8 bytes uint64_t)\n",
           (int)varintChainedVarintLen(123456));
    printf("   - Age 28: %d bytes (vs 4 bytes uint32_t)\n",
           (int)varintChainedVarintLen(28));

    // Frame prefix comparison
    printf("\n   Message framing (varintExternal):\n");
    printf("   - Length prefix for 50-byte message: 1 byte\n");
    printf("   - Length prefix for 500-byte message: 2 bytes\n");
    printf("   - vs fixed 4-byte length: saves 2-3 bytes per message\n");

    streamFree(&stream);

    printf("\n✓ Network protocol example complete\n");
}

int main(void) {
    printf("===========================================\n");
    printf("  Network Protocol Integration Example\n");
    printf("===========================================\n");

    demonstrateProtocol();

    printf("\n===========================================\n");
    printf("This example demonstrated:\n");
    printf("  • varintBitstream for bit-packed headers\n");
    printf("  • varintChained for Protocol Buffers\n");
    printf("  • varintExternal for length prefixes\n");
    printf("  • Custom protocol design\n");
    printf("  • Message framing\n");
    printf("===========================================\n");

    return 0;
}
