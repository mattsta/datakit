/*
 * Trie Server Client
 *
 * Simple client for testing the async trie server
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../../src/varint.h"
#include "../../src/varintTagged.h"

#define MAX_RESPONSE_SIZE 65536

typedef enum {
    CMD_ADD = 0x01,
    CMD_REMOVE = 0x02,
    CMD_SUBSCRIBE = 0x03,
    CMD_UNSUBSCRIBE = 0x04,
    CMD_MATCH = 0x05,
    CMD_LIST = 0x06,
    CMD_STATS = 0x07,
    CMD_SAVE = 0x08,
    CMD_PING = 0x09,
    CMD_AUTH = 0x0A,
    CMD_SHUTDOWN = 0x0B,
    // Enhanced pub/sub commands
    CMD_PUBLISH = 0x10,
    CMD_SUBSCRIBE_LIVE = 0x11,
    CMD_GET_SUBSCRIPTIONS = 0x12,
    CMD_SUBSCRIBE_BATCH = 0x13,
    CMD_SET_QOS = 0x14,
    CMD_ACK = 0x15,
    CMD_GET_BACKLOG = 0x16
} CommandType;

typedef enum {
    STATUS_OK = 0x00,
    STATUS_ERROR = 0x01,
    STATUS_AUTH_REQUIRED = 0x02,
    STATUS_RATE_LIMITED = 0x03,
    STATUS_INVALID_CMD = 0x04
} StatusCode;

typedef enum {
    MSG_NOTIFICATION = 0x80,
    MSG_SUBSCRIPTION_CONFIRM = 0x81,
    MSG_HEARTBEAT = 0x82
} MessageType;

typedef struct {
    int sockfd;
    char *host;
    uint16_t port;
    bool connected;
} TrieClient;

bool clientConnect(TrieClient *client, const char *host, uint16_t port) {
    client->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->sockfd < 0) {
        perror("socket");
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(client->sockfd);
        return false;
    }

    if (connect(client->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(client->sockfd);
        return false;
    }

    client->host = strdup(host);
    if (!client->host) {
        close(client->sockfd);
        return false;
    }

    client->port = port;
    client->connected = true;
    printf("Connected to %s:%d\n", host, port);
    return true;
}

void clientClose(TrieClient *client) {
    if (client->connected) {
        close(client->sockfd);
        client->connected = false;
    }
    if (client->host) {
        free(client->host);
        client->host = NULL;
    }
}

bool sendCommand(TrieClient *client, CommandType cmd, const uint8_t *payload,
                 size_t payloadLen) {
    if (!client->connected) {
        fprintf(stderr, "Not connected\n");
        return false;
    }

    // Allocate buffer on heap to avoid stack overflow
    const size_t bufferSize = 4096;
    uint8_t *buffer = (uint8_t *)malloc(bufferSize);
    if (!buffer) {
        fprintf(stderr, "Failed to allocate send buffer\n");
        return false;
    }

    // Build message: [Length:varint][CommandID:1byte][Payload]
    size_t offset = 0;

    // Reserve space for length
    offset += 5;

    // Command ID
    buffer[offset++] = (uint8_t)cmd;

    // Payload
    if (payload && payloadLen > 0) {
        memcpy(buffer + offset, payload, payloadLen);
        offset += payloadLen;
    }

    // Calculate message length
    uint64_t messageLen = offset - 5;

    // Write length
    size_t lengthBytes = varintTaggedPut64(buffer, messageLen);

    // Shift message
    memmove(buffer + lengthBytes, buffer + 5, messageLen);

    size_t totalSize = lengthBytes + messageLen;

    // Send
    ssize_t sent = write(client->sockfd, buffer, totalSize);
    free(buffer);

    if (sent != (ssize_t)totalSize) {
        perror("write");
        return false;
    }

    return true;
}

bool receiveResponse(TrieClient *client, StatusCode *status, uint8_t *data,
                     size_t *dataLen, size_t maxDataLen) {
    if (!client->connected) {
        return false;
    }

    // Read length
    uint8_t lengthBuf[9];
    size_t bytesRead = 0;
    uint64_t messageLen;

    // Read at least one byte
    if (read(client->sockfd, lengthBuf, 1) != 1) {
        return false;
    }
    bytesRead = 1;

    // Keep reading until we have a complete varint
    while (varintTaggedGet64(lengthBuf, &messageLen) == 0 &&
           bytesRead < sizeof(lengthBuf)) {
        if (read(client->sockfd, lengthBuf + bytesRead, 1) != 1) {
            return false;
        }
        bytesRead++;
    }

    if (messageLen == 0 || messageLen > MAX_RESPONSE_SIZE) {
        fprintf(stderr, "Invalid message length: %" PRIu64 "\n", messageLen);
        return false;
    }

    // Read message
    uint8_t *msgBuf = (uint8_t *)malloc(messageLen);
    if (!msgBuf) {
        return false;
    }

    size_t totalRead = 0;
    while (totalRead < messageLen) {
        ssize_t n =
            read(client->sockfd, msgBuf + totalRead, messageLen - totalRead);
        if (n <= 0) {
            free(msgBuf);
            return false;
        }
        totalRead += n;
    }

    // Parse response
    *status = (StatusCode)msgBuf[0];
    *dataLen = messageLen - 1;

    // CRITICAL: Bounds check before copying data
    if (*dataLen > maxDataLen) {
        fprintf(stderr, "Response data too large: %zu > %zu\n", *dataLen,
                maxDataLen);
        free(msgBuf);
        return false;
    }

    if (*dataLen > 0) {
        memcpy(data, msgBuf + 1, *dataLen);
    }

    free(msgBuf);
    return true;
}

bool cmdPing(TrieClient *client) {
    printf("Sending PING...\n");

    if (!sendCommand(client, CMD_PING, NULL, 0)) {
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        fprintf(stderr, "Failed to allocate response buffer\n");
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    free(data);

    if (status == STATUS_OK) {
        printf("PONG (OK)\n");
        return true;
    } else {
        printf("Error: status = 0x%02X\n", status);
        return false;
    }
}

bool cmdStats(TrieClient *client) {
    printf("Sending STATS...\n");

    if (!sendCommand(client, CMD_STATS, NULL, 0)) {
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        fprintf(stderr, "Failed to allocate response buffer\n");
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    if (status != STATUS_OK) {
        printf("Error: status = 0x%02X\n", status);
        free(data);
        return false;
    }

    // Parse stats
    size_t offset = 0;
    uint64_t patterns, subscribers, nodes, connections, commands, uptime;

    varintTaggedGet64(data + offset, &patterns);
    offset += varintTaggedGetLen(data + offset);

    varintTaggedGet64(data + offset, &subscribers);
    offset += varintTaggedGetLen(data + offset);

    varintTaggedGet64(data + offset, &nodes);
    offset += varintTaggedGetLen(data + offset);

    varintTaggedGet64(data + offset, &connections);
    offset += varintTaggedGetLen(data + offset);

    varintTaggedGet64(data + offset, &commands);
    offset += varintTaggedGetLen(data + offset);

    varintTaggedGet64(data + offset, &uptime);
    (void)offset; // Last use of offset

    printf("\nServer Statistics:\n");
    printf("  Patterns:     %" PRIu64 "\n", patterns);
    printf("  Subscribers:  %" PRIu64 "\n", subscribers);
    printf("  Nodes:        %" PRIu64 "\n", nodes);
    printf("  Connections:  %" PRIu64 "\n", connections);
    printf("  Commands:     %" PRIu64 "\n", commands);
    printf("  Uptime:       %" PRIu64 " seconds\n", uptime);

    free(data);
    return true;
}

bool cmdAdd(TrieClient *client, const char *pattern, uint32_t subscriberId,
            const char *subscriberName) {
    printf("Sending ADD pattern='%s' subscriberId=%u subscriberName='%s'...\n",
           pattern, subscriberId, subscriberName);

    // Build payload:
    // <pattern_len:varint><pattern:bytes><subscriber_id:varint><subscriber_name_len:varint><subscriber_name:bytes>
    uint8_t *payload = (uint8_t *)malloc(4096);
    if (!payload) {
        return false;
    }

    size_t offset = 0;
    size_t patternLen = strlen(pattern);
    offset += varintTaggedPut64(payload + offset, patternLen);
    memcpy(payload + offset, pattern, patternLen);
    offset += patternLen;

    offset += varintTaggedPut64(payload + offset, subscriberId);

    size_t nameLen = strlen(subscriberName);
    offset += varintTaggedPut64(payload + offset, nameLen);
    memcpy(payload + offset, subscriberName, nameLen);
    offset += nameLen;

    bool result = sendCommand(client, CMD_ADD, payload, offset);
    free(payload);

    if (!result) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    free(data);

    if (status == STATUS_OK) {
        printf("ADD successful\n");
        return true;
    } else {
        printf("Error: status = 0x%02X\n", status);
        return false;
    }
}

bool cmdRemove(TrieClient *client, const char *pattern) {
    printf("Sending REMOVE pattern='%s'...\n", pattern);

    uint8_t *payload = (uint8_t *)malloc(4096);
    if (!payload) {
        return false;
    }

    size_t offset = 0;
    size_t patternLen = strlen(pattern);
    offset += varintTaggedPut64(payload + offset, patternLen);
    memcpy(payload + offset, pattern, patternLen);
    offset += patternLen;

    bool result = sendCommand(client, CMD_REMOVE, payload, offset);
    free(payload);

    if (!result) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    free(data);

    if (status == STATUS_OK) {
        printf("REMOVE successful\n");
        return true;
    } else {
        printf("Error: status = 0x%02X\n", status);
        return false;
    }
}

bool cmdMatch(TrieClient *client, const char *input) {
    printf("Sending MATCH input='%s'...\n", input);

    uint8_t *payload = (uint8_t *)malloc(4096);
    if (!payload) {
        return false;
    }

    size_t offset = 0;
    size_t inputLen = strlen(input);
    offset += varintTaggedPut64(payload + offset, inputLen);
    memcpy(payload + offset, input, inputLen);
    offset += inputLen;

    bool result = sendCommand(client, CMD_MATCH, payload, offset);
    free(payload);

    if (!result) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    if (status != STATUS_OK) {
        printf("Error: status = 0x%02X\n", status);
        free(data);
        return false;
    }

    // Parse response:
    // <count:varint>[<subscriber_id:varint><subscriber_name_len:varint><subscriber_name:bytes>]*
    size_t respOffset = 0;
    uint64_t count;
    varintTaggedGet64(data + respOffset, &count);
    respOffset += varintTaggedGetLen(data + respOffset);

    printf("\nMatches found: %" PRIu64 "\n", count);
    for (uint64_t i = 0; i < count; i++) {
        uint64_t subscriberId;
        varintTaggedGet64(data + respOffset, &subscriberId);
        respOffset += varintTaggedGetLen(data + respOffset);

        uint64_t nameLen;
        varintTaggedGet64(data + respOffset, &nameLen);
        respOffset += varintTaggedGetLen(data + respOffset);

        char name[256];
        memcpy(name, data + respOffset, nameLen);
        name[nameLen] = '\0';
        respOffset += nameLen;

        printf("  [%" PRIu64 "] ID=%" PRIu64 " Name='%s'\n", i + 1,
               subscriberId, name);
    }

    free(data);
    return true;
}

bool cmdList(TrieClient *client) {
    printf("Sending LIST...\n");

    if (!sendCommand(client, CMD_LIST, NULL, 0)) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    if (status != STATUS_OK) {
        printf("Error: status = 0x%02X\n", status);
        free(data);
        return false;
    }

    // Parse response: <count:varint>[<pattern_len:varint><pattern:bytes>]*
    size_t respOffset = 0;
    uint64_t count;
    varintTaggedGet64(data + respOffset, &count);
    respOffset += varintTaggedGetLen(data + respOffset);

    printf("\nPatterns (%" PRIu64 " total):\n", count);
    for (uint64_t i = 0; i < count; i++) {
        uint64_t patternLen;
        varintTaggedGet64(data + respOffset, &patternLen);
        respOffset += varintTaggedGetLen(data + respOffset);

        char pattern[256];
        memcpy(pattern, data + respOffset, patternLen);
        pattern[patternLen] = '\0';
        respOffset += patternLen;

        printf("  %" PRIu64 ". %s\n", i + 1, pattern);
    }

    free(data);
    return true;
}

bool cmdSubscribe(TrieClient *client, const char *pattern,
                  uint32_t subscriberId, const char *subscriberName) {
    printf("Sending SUBSCRIBE pattern='%s' subscriberId=%u "
           "subscriberName='%s'...\n",
           pattern, subscriberId, subscriberName);

    // Build payload:
    // <pattern_len:varint><pattern:bytes><subscriber_id:varint><subscriber_name_len:varint><subscriber_name:bytes>
    uint8_t *payload = (uint8_t *)malloc(4096);
    if (!payload) {
        return false;
    }

    size_t offset = 0;
    size_t patternLen = strlen(pattern);
    offset += varintTaggedPut64(payload + offset, patternLen);
    memcpy(payload + offset, pattern, patternLen);
    offset += patternLen;

    offset += varintTaggedPut64(payload + offset, subscriberId);

    size_t nameLen = strlen(subscriberName);
    offset += varintTaggedPut64(payload + offset, nameLen);
    memcpy(payload + offset, subscriberName, nameLen);
    offset += nameLen;

    bool result = sendCommand(client, CMD_SUBSCRIBE, payload, offset);
    free(payload);

    if (!result) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    free(data);

    if (status == STATUS_OK) {
        printf("SUBSCRIBE successful\n");
        return true;
    } else {
        printf("Error: status = 0x%02X\n", status);
        return false;
    }
}

bool cmdUnsubscribe(TrieClient *client, const char *pattern,
                    uint32_t subscriberId) {
    printf("Sending UNSUBSCRIBE pattern='%s' subscriberId=%u...\n", pattern,
           subscriberId);

    // Build payload: <pattern_len:varint><pattern:bytes><subscriber_id:varint>
    uint8_t *payload = (uint8_t *)malloc(4096);
    if (!payload) {
        return false;
    }

    size_t offset = 0;
    size_t patternLen = strlen(pattern);
    offset += varintTaggedPut64(payload + offset, patternLen);
    memcpy(payload + offset, pattern, patternLen);
    offset += patternLen;

    offset += varintTaggedPut64(payload + offset, subscriberId);

    bool result = sendCommand(client, CMD_UNSUBSCRIBE, payload, offset);
    free(payload);

    if (!result) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    free(data);

    if (status == STATUS_OK) {
        printf("UNSUBSCRIBE successful\n");
        return true;
    } else {
        printf("Error: status = 0x%02X\n", status);
        return false;
    }
}

bool cmdSave(TrieClient *client) {
    printf("Sending SAVE...\n");

    if (!sendCommand(client, CMD_SAVE, NULL, 0)) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    free(data);

    if (status == STATUS_OK) {
        printf("SAVE successful\n");
        return true;
    } else {
        printf("Error: status = 0x%02X\n", status);
        return false;
    }
}

bool cmdAuth(TrieClient *client, const char *token) {
    printf("Sending AUTH...\n");

    // Build payload: <token_len:varint><token:bytes>
    uint8_t *payload = (uint8_t *)malloc(4096);
    if (!payload) {
        return false;
    }

    size_t offset = 0;
    size_t tokenLen = strlen(token);
    offset += varintTaggedPut64(payload + offset, tokenLen);
    memcpy(payload + offset, token, tokenLen);
    offset += tokenLen;

    bool result = sendCommand(client, CMD_AUTH, payload, offset);
    free(payload);

    if (!result) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    free(data);

    if (status == STATUS_OK) {
        printf("AUTH successful\n");
        return true;
    } else if (status == STATUS_AUTH_REQUIRED) {
        printf("AUTH failed: Invalid token\n");
        return false;
    } else {
        printf("Error: status = 0x%02X\n", status);
        return false;
    }
}

bool cmdShutdown(TrieClient *client) {
    printf("Sending SHUTDOWN...\n");

    if (!sendCommand(client, CMD_SHUTDOWN, NULL, 0)) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    free(data);

    if (status == STATUS_OK) {
        printf("SHUTDOWN successful - server will terminate gracefully\n");
        return true;
    } else {
        printf("Error: status = 0x%02X\n", status);
        return false;
    }
}

bool cmdPublish(TrieClient *client, const char *pattern, const char *message) {
    printf("Sending PUBLISH pattern='%s' message='%s'...\n", pattern, message);

    uint8_t *payload = (uint8_t *)malloc(4096);
    if (!payload) {
        return false;
    }

    size_t offset = 0;
    size_t patternLen = strlen(pattern);
    offset += varintTaggedPut64(payload + offset, patternLen);
    memcpy(payload + offset, pattern, patternLen);
    offset += patternLen;

    size_t messageLen = strlen(message);
    offset += varintTaggedPut64(payload + offset, messageLen);
    memcpy(payload + offset, message, messageLen);
    offset += messageLen;

    bool result = sendCommand(client, CMD_PUBLISH, payload, offset);
    free(payload);

    if (!result) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    free(data);

    if (status == STATUS_OK) {
        printf("PUBLISH successful\n");
        return true;
    } else {
        printf("Error: status = 0x%02X\n", status);
        return false;
    }
}

bool cmdSubscribeLive(TrieClient *client, const char *pattern, uint8_t qos,
                      uint32_t clientId, const char *clientName) {
    printf("Sending SUBSCRIBE_LIVE pattern='%s' qos=%d clientId=%u "
           "clientName='%s'...\n",
           pattern, qos, clientId, clientName ? clientName : "");

    uint8_t *payload = (uint8_t *)malloc(4096);
    if (!payload) {
        return false;
    }

    size_t offset = 0;
    size_t patternLen = strlen(pattern);
    offset += varintTaggedPut64(payload + offset, patternLen);
    memcpy(payload + offset, pattern, patternLen);
    offset += patternLen;

    payload[offset++] = qos;

    if (clientId > 0) {
        offset += varintTaggedPut64(payload + offset, clientId);
    }

    if (clientName && strlen(clientName) > 0) {
        size_t nameLen = strlen(clientName);
        offset += varintTaggedPut64(payload + offset, nameLen);
        memcpy(payload + offset, clientName, nameLen);
        offset += nameLen;
    }

    bool result = sendCommand(client, CMD_SUBSCRIBE_LIVE, payload, offset);
    free(payload);

    if (!result) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    if (status == STATUS_OK) {
        if (dataLen > 0) {
            uint64_t assignedClientId;
            varintTaggedGet64(data, &assignedClientId);
            printf("SUBSCRIBE_LIVE successful - Assigned Client ID: %" PRIu64
                   "\n",
                   assignedClientId);
        } else {
            printf("SUBSCRIBE_LIVE successful\n");
        }
        free(data);
        return true;
    } else {
        printf("Error: status = 0x%02X\n", status);
        free(data);
        return false;
    }
}

bool cmdGetSubscriptions(TrieClient *client) {
    printf("Sending GET_SUBSCRIPTIONS...\n");

    if (!sendCommand(client, CMD_GET_SUBSCRIPTIONS, NULL, 0)) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    if (status != STATUS_OK) {
        printf("Error: status = 0x%02X\n", status);
        free(data);
        return false;
    }

    size_t offset = 0;
    uint64_t count;
    varintTaggedGet64(data + offset, &count);
    offset += varintTaggedGetLen(data + offset);

    printf("\nActive Subscriptions (%" PRIu64 " total):\n", count);
    for (uint64_t i = 0; i < count; i++) {
        uint64_t patternLen;
        varintTaggedGet64(data + offset, &patternLen);
        offset += varintTaggedGetLen(data + offset);

        char pattern[256];
        memcpy(pattern, data + offset, patternLen);
        pattern[patternLen] = '\0';
        offset += patternLen;

        uint8_t qos = data[offset++];

        printf("  %" PRIu64 ". Pattern='%s' QoS=%d\n", i + 1, pattern, qos);
    }

    free(data);
    return true;
}

bool listenForNotifications(TrieClient *client, int timeout_seconds) {
    printf("Listening for notifications (timeout=%d seconds)...\n",
           timeout_seconds);
    printf("Press Ctrl+C to stop\n\n");

    time_t startTime = time(NULL);
    bool receivedAny = false;

    while (true) {
        // Check timeout
        if (timeout_seconds > 0 && time(NULL) - startTime > timeout_seconds) {
            printf("\nTimeout reached\n");
            break;
        }

        // Try to receive message (non-blocking)
        uint8_t lengthBuf[9];
        ssize_t n = read(client->sockfd, lengthBuf, 1);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec ts = {0, 100000000}; // 100ms
                nanosleep(&ts, NULL);
                continue;
            }
            perror("read");
            break;
        } else if (n == 0) {
            printf("Connection closed by server\n");
            break;
        }

        // Read rest of varint length
        size_t bytesRead = 1;
        uint64_t messageLen;
        while (varintTaggedGet64(lengthBuf, &messageLen) == 0 &&
               bytesRead < sizeof(lengthBuf)) {
            if (read(client->sockfd, lengthBuf + bytesRead, 1) != 1) {
                return receivedAny;
            }
            bytesRead++;
        }

        if (messageLen == 0 || messageLen > MAX_RESPONSE_SIZE) {
            fprintf(stderr, "Invalid message length: %" PRIu64 "\n",
                    messageLen);
            break;
        }

        // Read message
        uint8_t *msgBuf = (uint8_t *)malloc(messageLen);
        if (!msgBuf) {
            break;
        }

        size_t totalRead = 0;
        while (totalRead < messageLen) {
            n = read(client->sockfd, msgBuf + totalRead,
                     messageLen - totalRead);
            if (n <= 0) {
                free(msgBuf);
                return receivedAny;
            }
            totalRead += n;
        }

        // Parse message type
        uint8_t msgType = msgBuf[0];

        if (msgType == MSG_NOTIFICATION) {
            receivedAny = true;
            size_t offset = 1;

            // Parse notification
            uint64_t seqNum;
            varintTaggedGet64(msgBuf + offset, &seqNum);
            offset += varintTaggedGetLen(msgBuf + offset);

            uint64_t patternLen;
            varintTaggedGet64(msgBuf + offset, &patternLen);
            offset += varintTaggedGetLen(msgBuf + offset);

            char pattern[256];
            memcpy(pattern, msgBuf + offset, patternLen);
            pattern[patternLen] = '\0';
            offset += patternLen;

            uint64_t publisherId;
            varintTaggedGet64(msgBuf + offset, &publisherId);
            offset += varintTaggedGetLen(msgBuf + offset);

            uint64_t publisherNameLen;
            varintTaggedGet64(msgBuf + offset, &publisherNameLen);
            offset += varintTaggedGetLen(msgBuf + offset);

            char publisherName[256];
            memcpy(publisherName, msgBuf + offset, publisherNameLen);
            publisherName[publisherNameLen] = '\0';
            offset += publisherNameLen;

            uint64_t payloadLen;
            varintTaggedGet64(msgBuf + offset, &payloadLen);
            offset += varintTaggedGetLen(msgBuf + offset);

            char payload[1024];
            if (payloadLen < sizeof(payload)) {
                memcpy(payload, msgBuf + offset, payloadLen);
                payload[payloadLen] = '\0';
            } else {
                strcpy(payload, "[payload too large]");
            }

            printf("[NOTIFICATION] seq=%" PRIu64 " pattern='%s' from='%s' "
                   "(id=%" PRIu64 "): %s\n",
                   seqNum, pattern, publisherName, publisherId, payload);

            // Send ACK if QoS=1
            // For simplicity, always ACK
            uint8_t ackPayload[16];
            size_t ackLen = varintTaggedPut64(ackPayload, seqNum);
            sendCommand(client, CMD_ACK, ackPayload, ackLen);
        } else if (msgType == MSG_HEARTBEAT) {
            printf("[HEARTBEAT]\n");
        } else {
            printf("[UNKNOWN MESSAGE TYPE: 0x%02X]\n", msgType);
        }

        free(msgBuf);
    }

    return receivedAny;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <command> [args] [host] [port]\n", argv[0]);
        printf("\nLegacy Commands:\n");
        printf("  ping                                        - Send PING "
               "command\n");
        printf("  stats                                       - Get server "
               "statistics\n");
        printf("  add <pattern> <id> <name>                   - Add pattern "
               "with subscriber\n");
        printf(
            "  remove <pattern>                            - Remove pattern\n");
        printf("  subscribe <pattern> <id> <name>             - Subscribe to "
               "pattern (legacy)\n");
        printf("  unsubscribe <pattern> <id>                  - Unsubscribe "
               "from pattern\n");
        printf("  match <input>                               - Match input "
               "against patterns\n");
        printf("  list                                        - List all "
               "patterns\n");
        printf("  save                                        - Trigger manual "
               "save\n");
        printf("  auth <token>                                - Authenticate "
               "with token\n");
        printf("  shutdown                                    - Gracefully "
               "shutdown server\n");
        printf("\nNew Pub/Sub Commands:\n");
        printf("  publish <pattern> <message>                 - Publish "
               "message to pattern\n");
        printf("  sub-live <pattern> [qos] [id] [name]       - Subscribe with "
               "live notifications\n");
        printf("  listen [timeout]                            - Listen for "
               "notifications\n");
        printf("  get-subs                                    - Get current "
               "subscriptions\n");
        printf("\nDefault host: 127.0.0.1\n");
        printf("Default port: 9999\n");
        printf("\nExamples:\n");
        printf("  %s sub-live \"sensors.*.temperature\" 1 0 \"temp-monitor\"\n",
               argv[0]);
        printf("  %s listen 60\n", argv[0]);
        printf("  %s publish \"sensors.room1.temperature\" \"25.5C\"\n",
               argv[0]);
        printf("  %s get-subs\n", argv[0]);
        return 1;
    }

    const char *command = argv[1];

    // Determine host and port based on command
    const char *host;
    uint16_t port;

    if (strcmp(command, "add") == 0 || strcmp(command, "subscribe") == 0) {
        if (argc < 5) {
            fprintf(stderr,
                    "Usage: %s %s <pattern> <id> <name> [host] [port]\n",
                    argv[0], command);
            return 1;
        }
        host = argc > 5 ? argv[5] : "127.0.0.1";
        port = argc > 6 ? atoi(argv[6]) : 9999;
    } else if (strcmp(command, "remove") == 0 ||
               strcmp(command, "match") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s %s <pattern> [host] [port]\n", argv[0],
                    command);
            return 1;
        }
        host = argc > 3 ? argv[3] : "127.0.0.1";
        port = argc > 4 ? atoi(argv[4]) : 9999;
    } else if (strcmp(command, "unsubscribe") == 0) {
        if (argc < 4) {
            fprintf(stderr,
                    "Usage: %s unsubscribe <pattern> <id> [host] [port]\n",
                    argv[0]);
            return 1;
        }
        host = argc > 4 ? argv[4] : "127.0.0.1";
        port = argc > 5 ? atoi(argv[5]) : 9999;
    } else if (strcmp(command, "auth") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s auth <token> [host] [port]\n", argv[0]);
            return 1;
        }
        host = argc > 3 ? argv[3] : "127.0.0.1";
        port = argc > 4 ? atoi(argv[4]) : 9999;
    } else {
        host = argc > 2 ? argv[2] : "127.0.0.1";
        port = argc > 3 ? atoi(argv[3]) : 9999;
    }

    TrieClient client = {0};

    if (!clientConnect(&client, host, port)) {
        fprintf(stderr, "Failed to connect to server\n");
        return 1;
    }

    bool success = false;

    if (strcmp(command, "ping") == 0) {
        success = cmdPing(&client);
    } else if (strcmp(command, "stats") == 0) {
        success = cmdStats(&client);
    } else if (strcmp(command, "add") == 0) {
        success = cmdAdd(&client, argv[2], atoi(argv[3]), argv[4]);
    } else if (strcmp(command, "remove") == 0) {
        success = cmdRemove(&client, argv[2]);
    } else if (strcmp(command, "subscribe") == 0) {
        success = cmdSubscribe(&client, argv[2], atoi(argv[3]), argv[4]);
    } else if (strcmp(command, "unsubscribe") == 0) {
        success = cmdUnsubscribe(&client, argv[2], atoi(argv[3]));
    } else if (strcmp(command, "match") == 0) {
        success = cmdMatch(&client, argv[2]);
    } else if (strcmp(command, "list") == 0) {
        success = cmdList(&client);
    } else if (strcmp(command, "save") == 0) {
        success = cmdSave(&client);
    } else if (strcmp(command, "auth") == 0) {
        success = cmdAuth(&client, argv[2]);
    } else if (strcmp(command, "shutdown") == 0) {
        success = cmdShutdown(&client);
    } else if (strcmp(command, "publish") == 0) {
        if (argc < 4) {
            fprintf(stderr,
                    "Usage: %s publish <pattern> <message> [host] [port]\n",
                    argv[0]);
            clientClose(&client);
            return 1;
        }
        success = cmdPublish(&client, argv[2], argv[3]);
    } else if (strcmp(command, "sub-live") == 0) {
        if (argc < 3) {
            fprintf(stderr,
                    "Usage: %s sub-live <pattern> [qos] [id] [name] [host] "
                    "[port]\n",
                    argv[0]);
            clientClose(&client);
            return 1;
        }
        uint8_t qos = argc > 3 ? atoi(argv[3]) : 0;
        uint32_t clientId = argc > 4 ? atoi(argv[4]) : 0;
        const char *clientName = argc > 5 ? argv[5] : "client";
        success = cmdSubscribeLive(&client, argv[2], qos, clientId, clientName);
        if (success) {
            // Stay connected and listen for notifications
            success = listenForNotifications(&client, 0); // No timeout
        }
    } else if (strcmp(command, "listen") == 0) {
        int timeout = argc > 2 ? atoi(argv[2]) : 0;
        success = listenForNotifications(&client, timeout);
    } else if (strcmp(command, "get-subs") == 0) {
        success = cmdGetSubscriptions(&client);
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
    }

    clientClose(&client);
    return success ? 0 : 1;
}
