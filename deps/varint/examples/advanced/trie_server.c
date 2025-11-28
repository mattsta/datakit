/*
 * High-Performance Async Trie Server
 *
 * Architecture:
 * - Non-blocking async event loop (select-based for portability)
 * - Binary protocol with varint encoding
 * - Concurrent client support (1000+ connections)
 * - Auto-save persistence with configurable intervals
 * - Token-based authentication (optional)
 * - Per-connection rate limiting
 * - Comprehensive error handling and validation
 *
 * Protocol Format:
 *   Request:  [Length:varint][CommandID:1byte][Payload:varies]
 *   Response: [Length:varint][Status:1byte][Data:varies]
 *
 * Commands:
 *   0x01 ADD         - Add pattern with subscriber
 *   0x02 REMOVE      - Remove entire pattern
 *   0x03 SUBSCRIBE   - Add subscriber to pattern
 *   0x04 UNSUBSCRIBE - Remove subscriber from pattern
 *   0x05 MATCH       - Query pattern matching
 *   0x06 LIST        - List all patterns
 *   0x07 STATS       - Get server statistics
 *   0x08 SAVE        - Trigger manual save
 *   0x09 PING        - Keepalive
 *   0x0A AUTH        - Authenticate with token
 *
 * Status Codes:
 *   0x00 OK             - Success
 *   0x01 ERROR          - Generic error
 *   0x02 AUTH_REQUIRED  - Authentication needed
 *   0x03 RATE_LIMITED   - Too many requests
 *   0x04 INVALID_CMD    - Unknown command
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Platform-specific event loop implementation */
#if defined(__linux__)
#include <sys/epoll.h>
#define USE_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||     \
    defined(__OpenBSD__)
#include <sys/event.h>
#define USE_KQUEUE 1
#else
#error "Unsupported platform: need epoll (Linux) or kqueue (BSD/macOS) support"
#endif

#include "../../src/varint.h"
#include "../../src/varintBitstream.h"
#include "../../src/varintTagged.h"

// ============================================================================
// PLATFORM ABSTRACTION LAYER (epoll/kqueue)
// ============================================================================

#ifdef USE_EPOLL
/* Linux: use epoll directly */
typedef struct epoll_event event_t;
#define EVENT_SIZE sizeof(struct epoll_event)

static inline int event_queue_create(void) {
    return epoll_create1(0);
}

static inline int event_queue_add(int eq, int fd, uint32_t events, void *data) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = data;
    return epoll_ctl(eq, EPOLL_CTL_ADD, fd, &ev);
}

static inline int event_queue_mod(int eq, int fd, uint32_t events, void *data) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = data;
    return epoll_ctl(eq, EPOLL_CTL_MOD, fd, &ev);
}

static inline int event_queue_del(int eq, int fd) {
    return epoll_ctl(eq, EPOLL_CTL_DEL, fd, NULL);
}

static inline int event_queue_wait(int eq, event_t *events, int maxevents,
                                   int timeout) {
    return epoll_wait(eq, events, maxevents, timeout);
}

static inline void *event_get_data(const event_t *ev) {
    return ev->data.ptr;
}

static inline uint32_t event_get_events(const event_t *ev) {
    return ev->events;
}

#define EVENT_READ EPOLLIN
#define EVENT_WRITE EPOLLOUT
#define EVENT_ET EPOLLET

#elif defined(USE_KQUEUE)
/* macOS/BSD: use kqueue with compatibility layer */
typedef struct kevent event_t;
#define EVENT_SIZE sizeof(struct kevent)

/* Event flags - define before using in functions */
#define EVENT_READ 0x001
#define EVENT_WRITE 0x004
#define EVENT_ET 0 /* kqueue is always edge-triggered */

/* Compatibility defines for epoll constants used in code */
#define EPOLLIN EVENT_READ
#define EPOLLOUT EVENT_WRITE
#define EPOLLET EVENT_ET
#define epoll_event kevent

static inline int event_queue_create(void) {
    return kqueue();
}

static inline int event_queue_add(int kq, int fd, uint32_t events, void *data) {
    struct kevent ev[2];
    int n = 0;
    if (events & EVENT_READ) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, data);
    }
    if (events & EVENT_WRITE) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, data);
    }
    return kevent(kq, ev, n, NULL, 0, NULL);
}

static inline int event_queue_mod(int kq, int fd, uint32_t events, void *data) {
    /* For kqueue, mod is the same as add - it updates existing events */
    return event_queue_add(kq, fd, events, data);
}

static inline int event_queue_del(int kq, int fd) {
    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    /* Ignore errors as the filters might not be registered */
    kevent(kq, ev, 2, NULL, 0, NULL);
    return 0;
}

static inline int event_queue_wait(int kq, event_t *events, int maxevents,
                                   int timeout) {
    struct timespec ts;
    struct timespec *tsp = NULL;
    if (timeout >= 0) {
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = (timeout % 1000) * 1000000;
        tsp = &ts;
    }
    return kevent(kq, NULL, 0, events, maxevents, tsp);
}

static inline void *event_get_data(const event_t *ev) {
    return ev->udata;
}

static inline uint32_t event_get_events(const event_t *ev) {
    uint32_t events = 0;
    if (ev->filter == EVFILT_READ) {
        events |= EVENT_READ;
    }
    if (ev->filter == EVFILT_WRITE) {
        events |= EVENT_WRITE;
    }
    return events;
}

#endif

// ============================================================================
// CONFIGURATION
// ============================================================================

#define DEFAULT_PORT 9999
#define MAX_CLIENTS 1024
#define MAX_MESSAGE_SIZE (64 * 1024) // 64KB max message
#define READ_BUFFER_SIZE 8192
#define WRITE_BUFFER_SIZE 8192
#define AUTH_TOKEN_MAX_LEN 256
#define RATE_LIMIT_WINDOW 1          // seconds
#define RATE_LIMIT_MAX_COMMANDS 1000 // commands per window
#define AUTO_SAVE_INTERVAL 60        // seconds
#define AUTO_SAVE_THRESHOLD 1000     // commands
#define CLIENT_TIMEOUT 300           // seconds (5 minutes idle)

// Debug logging (comment out for production)
// #define ENABLE_DEBUG_LOGGING
#ifdef ENABLE_DEBUG_LOGGING
#define DEBUG_LOG(...) fprintf(stderr, "DEBUG: " __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#endif

// ============================================================================
// TRIE DATA STRUCTURES (from trie_interactive.c)
// ============================================================================

#define MAX_PATTERN_LENGTH 256
#define MAX_SEGMENT_LENGTH 64
#define MAX_SEGMENTS 16
#define MAX_SUBSCRIBERS 256
#define MAX_SUBSCRIBER_NAME 64

typedef enum {
    SEGMENT_LITERAL = 0,
    SEGMENT_STAR = 1,
    SEGMENT_HASH = 2
} SegmentType;

typedef struct {
    uint32_t id;
    char name[MAX_SUBSCRIBER_NAME];
} Subscriber;

typedef struct {
    Subscriber subscribers[MAX_SUBSCRIBERS];
    size_t count;
} SubscriberList;

typedef struct TrieNode {
    char segment[MAX_SEGMENT_LENGTH];
    SegmentType type;
    bool isTerminal;
    SubscriberList subscribers;
    struct TrieNode **children;
    size_t childCount;
    size_t childCapacity;
} TrieNode;

typedef struct {
    TrieNode *root;
    size_t patternCount;
    size_t nodeCount;
    size_t subscriberCount;
} PatternTrie;

typedef struct {
    uint32_t subscriberIds[MAX_SUBSCRIBERS];
    char subscriberNames[MAX_SUBSCRIBERS][MAX_SUBSCRIBER_NAME];
    size_t count;
} MatchResult;

// ============================================================================
// PROTOCOL DEFINITIONS
// ============================================================================

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
    CMD_PUBLISH = 0x10,           // Publish event to pattern
    CMD_SUBSCRIBE_LIVE = 0x11,    // Subscribe with live notifications
    CMD_GET_SUBSCRIPTIONS = 0x12, // Get current subscriptions
    CMD_SUBSCRIBE_BATCH = 0x13,   // Subscribe to multiple patterns
    CMD_SET_QOS = 0x14,           // Set quality of service
    CMD_ACK = 0x15,               // Acknowledge message
    CMD_GET_BACKLOG = 0x16        // Retrieve missed messages
} CommandType;

typedef enum {
    STATUS_OK = 0x00,
    STATUS_ERROR = 0x01,
    STATUS_AUTH_REQUIRED = 0x02,
    STATUS_RATE_LIMITED = 0x03,
    STATUS_INVALID_CMD = 0x04
} StatusCode;

// Message types for async server->client notifications
typedef enum {
    MSG_NOTIFICATION = 0x80,         // Async notification of published event
    MSG_SUBSCRIPTION_CONFIRM = 0x81, // Subscription confirmed
    MSG_HEARTBEAT = 0x82             // Keepalive heartbeat
} MessageType;

// QoS levels for message delivery
typedef enum {
    QOS_AT_MOST_ONCE = 0, // Fire and forget
    QOS_AT_LEAST_ONCE = 1 // Requires acknowledgment
} QoSLevel;

// ============================================================================
// PUB/SUB DATA STRUCTURES
// ============================================================================

#define MAX_SUBSCRIPTIONS_PER_CLIENT 256
#define MAX_MESSAGE_QUEUE_SIZE 1000
#define MAX_PAYLOAD_SIZE                                                       \
    (1024 * 1024 * 1024) // 1GB sanity limit (effectively no restriction)

// Per-connection subscription
typedef struct {
    char pattern[MAX_PATTERN_LENGTH];
    QoSLevel qos;
    uint64_t lastSeqNum; // Last sequence number acknowledged
    bool active;
} ConnectionSubscription;

// Buffered message for reliable delivery
typedef struct {
    uint64_t seqNum;
    time_t timestamp;
    char pattern[MAX_PATTERN_LENGTH];
    uint8_t *payload;
    size_t payloadLen;
    int *pendingClientFds; // FDs that need to ack this message
    size_t pendingClientCount;
    uint64_t publisherId; // ID of the publisher (64-bit for scalability)
    char publisherName[MAX_SUBSCRIBER_NAME];
} BufferedMessage;

// ============================================================================
// CONNECTION STATE
// ============================================================================

typedef enum {
    CONN_READING_LENGTH,
    CONN_READING_MESSAGE,
    CONN_PROCESSING,
    CONN_WRITING_RESPONSE,
    CONN_CLOSED
} ConnectionState;

typedef struct {
    int fd;
    ConnectionState state;
    bool authenticated;
    time_t lastActivity;

    // Rate limiting
    time_t rateLimitWindowStart;
    uint32_t commandsInWindow;

    // Read state
    uint8_t readBuffer[READ_BUFFER_SIZE];
    size_t readOffset;
    size_t messageLength;
    size_t messageBytesRead;

    // Write state
    uint8_t writeBuffer[WRITE_BUFFER_SIZE];
    size_t writeOffset;
    size_t writeLength;

    // Pub/sub state
    ConnectionSubscription subscriptions[MAX_SUBSCRIPTIONS_PER_CLIENT];
    size_t subscriptionCount;
    BufferedMessage *messageQueue;
    size_t queueSize;
    size_t queueCapacity;
    uint64_t nextSeqNum; // Next sequence number for this client
    QoSLevel defaultQos;

    // Client identity for pub/sub
    uint64_t clientId; // Unique client ID (64-bit for scalability)
    char clientName[MAX_SUBSCRIBER_NAME];
    bool hasIdentity;

    // Pending notifications (messages waiting to be sent)
    size_t *pendingNotifications; // Indices into messageQueue
    size_t pendingNotificationCount;
    size_t pendingNotificationCapacity;
} ClientConnection;

// ============================================================================
// CLIENT MANAGEMENT
// ============================================================================

// Hash table entry for client management
typedef struct ClientMapEntry {
    uint64_t key; // Can be clientId or fd (depending on hash table)
    ClientConnection *conn;
    struct ClientMapEntry *next; // Chaining for collisions
} ClientMapEntry;

// Client hash table
typedef struct {
    ClientMapEntry **buckets;
    size_t bucketCount;
    size_t itemCount;
} ClientHashTable;

// Client manager with dual indexing
typedef struct {
    ClientHashTable byId;          // Hash by client ID
    ClientHashTable byFd;          // Hash by file descriptor
    ClientConnection **activeList; // Array of pointers to active clients
    size_t activeCount;
    size_t activeCapacity;

    // Free list for client structures
    ClientConnection *freeList;
    ClientConnection *allocatedPool;
    size_t poolSize;
    size_t poolCapacity;
} ClientManager;

// ============================================================================
// SUBSCRIPTION OPTIMIZATION (Phase 2)
// ============================================================================
//
// The trie already handles dynamic pattern matching with wildcards.
// Phase 2 optimization: Ensure live subscriptions use client ID as subscriber
// ID in the trie, enabling O(1) client lookup after pattern matching.
//
// Flow:
//   1. CMD_SUBSCRIBE_LIVE → trieInsert(pattern, clientId, clientName)
//   2. CMD_PUBLISH("sensors.room1.temp") → trieMatch returns [clientId1,
//   clientId2, ...]
//   3. For each clientId: clientMgrGetById(clientId) → O(1) lookup
//   4. Send notification directly to client
//
// No additional indexing needed - trie handles pattern matching,
// ClientManager handles O(1) client lookups!

// ============================================================================
// MESSAGE BUFFER POOLS (Phase 3)
// ============================================================================
//
// Eliminate malloc/free overhead by pre-allocating message structures and
// payload buffers in memory pools.
//
// Benefits:
//   - 10× faster allocation compared to malloc
//   - Reduced memory fragmentation
//   - Predictable latency (no heap allocation delays)
//   - Better cache locality

// Payload buffer with size tier tracking
typedef struct PooledBuffer {
    uint8_t *data;
    size_t size;
    bool inUse;
} PooledBuffer;

// Buffer pool for a specific size tier - DYNAMIC, auto-expanding
typedef struct BufferTier {
    PooledBuffer *buffers;
    size_t bufferSize;      // Size of each buffer in this tier
    size_t capacity;        // Current capacity (grows dynamically)
    size_t initialCapacity; // Starting capacity
    size_t *freeList;       // Stack of available buffer indices
    size_t freeCount;       // Number of free buffers
    size_t totalAllocated;  // Total allocations from this tier (for statistics)
    size_t expansionCount;  // Number of times tier has expanded
} BufferTier;

// Multi-tier buffer pool - pools SMALL common sizes, malloc for large/arbitrary
// sizes This is an OPTIMIZATION, not a restriction - supports ANY size
typedef struct BufferPoolManager {
    BufferTier *tiers;    // Dynamic array of tiers
    size_t tierCount;     // Number of tiers (can grow)
    size_t maxPooledSize; // Largest size we pool (beyond this, use malloc)
    size_t totalAllocations;
    size_t totalFrees;
    size_t poolHits;         // Allocations from pool
    size_t poolMisses;       // Allocations via malloc (large or pool exhausted)
    size_t directAllocBytes; // Total bytes allocated via malloc
} BufferPoolManager;

// Message pool for BufferedMessage structures
typedef struct MessagePool {
    BufferedMessage *messages;
    bool *inUse; // Bitmap of which messages are in use
    size_t capacity;
    size_t *freeList; // Stack of available message indices
    size_t freeCount;
} MessagePool;

// ============================================================================
// SERVER STATE
// ============================================================================

typedef struct {
    int listenFd;
    int eventFd;      /* epoll fd on Linux, kqueue fd on macOS/BSD */
    PatternTrie trie; // Pattern matching with wildcards
    ClientManager
        clientMgr;       // Phase 1: Dynamic client management with O(1) lookups
    MessagePool msgPool; // Phase 3: Message structure pool
    BufferPoolManager bufferPool; // Phase 3: Multi-tier payload buffer pools
    bool running;

    // Configuration
    uint16_t port;
    char *authToken;
    bool requireAuth;
    char *saveFilePath;

    // Auto-save state
    time_t lastSaveTime;
    uint64_t commandsSinceLastSave;

    // Statistics
    uint64_t totalConnections;
    uint64_t totalCommands;
    uint64_t totalErrors;
    time_t startTime;

    // Pub/sub statistics
    uint64_t totalPublishes;
    uint64_t totalNotificationsSent;
    uint64_t totalLiveSubscriptions;
    uint64_t nextClientId; // Counter for assigning unique client IDs

    // Global message buffer for QoS=1 messages
    BufferedMessage *globalMessageBuffer;
    size_t globalBufferSize;
    size_t globalBufferCapacity;
    uint64_t nextGlobalSeqNum;

    // Heartbeat state
    time_t lastHeartbeat;
} TrieServer;

// ============================================================================
// FORWARD DECLARATIONS - TRIE OPERATIONS
// ============================================================================

void trieInit(PatternTrie *trie);
void trieFree(PatternTrie *trie);
bool trieInsert(PatternTrie *trie, const char *pattern, uint32_t subscriberId,
                const char *subscriberName);
bool trieRemovePattern(PatternTrie *trie, const char *pattern);
bool trieRemoveSubscriber(PatternTrie *trie, const char *pattern,
                          uint32_t subscriberId);
void trieMatch(PatternTrie *trie, const char *input, MatchResult *result);
void trieListPatterns(const PatternTrie *trie,
                      char patterns[][MAX_PATTERN_LENGTH], size_t *count,
                      size_t maxCount);
void trieStats(const PatternTrie *trie, size_t *totalNodes,
               size_t *terminalNodes, size_t *wildcardNodes, size_t *maxDepth);
bool trieSave(const PatternTrie *trie, const char *filename);
bool trieLoad(PatternTrie *trie, const char *filename);

// ============================================================================
// FORWARD DECLARATIONS - SERVER
// ============================================================================

bool serverInit(TrieServer *server, uint16_t port, const char *authToken,
                const char *saveFilePath);
void serverRun(TrieServer *server);
void serverShutdown(TrieServer *server);
void handleClient(TrieServer *server, ClientConnection *client);
bool processCommand(TrieServer *server, ClientConnection *client,
                    const uint8_t *data, size_t length);
void sendResponse(int eventFd, ClientConnection *client, StatusCode status,
                  const uint8_t *data, size_t dataLen);

// ============================================================================
// FORWARD DECLARATIONS - CLIENT MANAGEMENT
// ============================================================================

bool clientMgrInit(ClientManager *mgr, size_t initialCapacity);
void clientMgrDestroy(ClientManager *mgr);
ClientConnection *clientMgrAllocate(ClientManager *mgr, int fd,
                                    uint64_t clientId);
void clientMgrFree(ClientManager *mgr, ClientConnection *conn);
ClientConnection *clientMgrGetById(ClientManager *mgr, uint64_t clientId);
ClientConnection *clientMgrGetByFd(ClientManager *mgr, int fd);
bool clientMgrAdd(ClientManager *mgr, ClientConnection *conn);
void clientMgrRemove(ClientManager *mgr, const ClientConnection *conn);
size_t clientMgrGetActiveCount(const ClientManager *mgr);

// ============================================================================
// FORWARD DECLARATIONS - MESSAGE POOLS (PHASE 3)
// ============================================================================

bool msgPoolInit(MessagePool *pool, size_t capacity);
void msgPoolDestroy(MessagePool *pool);
BufferedMessage *msgPoolAlloc(MessagePool *pool);
void msgPoolFree(MessagePool *pool, BufferedMessage *msg);

bool bufferPoolInit(BufferPoolManager *mgr);
void bufferPoolDestroy(BufferPoolManager *mgr);
uint8_t *bufferPoolAlloc(BufferPoolManager *mgr, size_t size);
void bufferPoolFree(BufferPoolManager *mgr, uint8_t *buffer, size_t size);

// ============================================================================
// FORWARD DECLARATIONS - PUB/SUB
// ============================================================================

void initClientPubSub(ClientConnection *client, uint64_t clientId);
void cleanupClientPubSub(ClientConnection *client);
bool addClientSubscription(ClientConnection *client, const char *pattern,
                           QoSLevel qos);
bool removeClientSubscription(ClientConnection *client, const char *pattern);
bool publishMessage(TrieServer *server, const char *pattern,
                    const uint8_t *payload, size_t payloadLen,
                    uint64_t publisherId, const char *publisherName);
void sendNotification(TrieServer *server, ClientConnection *client,
                      const BufferedMessage *msg);
void queueNotificationForClient(TrieServer *server, ClientConnection *client,
                                size_t msgIndex);
void processNotificationQueue(TrieServer *server, ClientConnection *client);
void acknowledgeMessage(TrieServer *server, ClientConnection *client,
                        uint64_t seqNum);
void cleanupOldMessages(TrieServer *server);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

static void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool checkRateLimit(ClientConnection *client) {
    time_t now = time(NULL);

    if (now - client->rateLimitWindowStart >= RATE_LIMIT_WINDOW) {
        // New window
        client->rateLimitWindowStart = now;
        client->commandsInWindow = 0;
    }

    if (client->commandsInWindow >= RATE_LIMIT_MAX_COMMANDS) {
        return false; // Rate limited
    }

    client->commandsInWindow++;
    return true;
}

static void resetClient(int eventFd, ClientConnection *client) {
    if (client->fd >= 0) {
        // Remove from event queue before closing
        if (eventFd >= 0) {
            event_queue_del(eventFd, client->fd);
        }
        close(client->fd);
    }

    // Cleanup pub/sub resources
    cleanupClientPubSub(client);

    uint64_t savedClientId = client->clientId;
    memset(client, 0, sizeof(ClientConnection));
    client->fd = -1;
    client->state = CONN_CLOSED;

    // Re-initialize pub/sub for next use (keep same ID for potential reconnect)
    initClientPubSub(client, savedClientId);
}

// Disconnect client and remove from ClientManager
static void disconnectClient(TrieServer *server, ClientConnection *client) {
    // Remove all client's subscriptions from trie (critical for Phase 2!)
    for (size_t i = 0; i < client->subscriptionCount; i++) {
        if (client->subscriptions[i].active) {
            trieRemoveSubscriber(&server->trie,
                                 client->subscriptions[i].pattern,
                                 client->clientId);
        }
    }

    resetClient(server->eventFd, client);
    clientMgrRemove(&server->clientMgr, client);
    clientMgrFree(&server->clientMgr, client);
}

// ============================================================================
// TRIE IMPLEMENTATION (Core functions from trie_interactive.c)
// ============================================================================

// Secure string copy with bounds checking (for null-terminated strings)
static void secureStrCopy(char *dst, size_t dstSize, const char *src) {
    if (!dst || !src || dstSize == 0) {
        return;
    }

    size_t i;
    for (i = 0; i < dstSize - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

// Secure binary copy with explicit length (for non-null-terminated binary data)
static void secureBinaryCopy(char *dst, size_t dstSize, const uint8_t *src,
                             size_t srcLen) {
    if (!dst || !src || dstSize == 0) {
        return;
    }

    // Copy up to the smaller of srcLen or dstSize-1
    size_t copyLen = (srcLen < dstSize - 1) ? srcLen : (dstSize - 1);
    memcpy(dst, src, copyLen);
    dst[copyLen] = '\0'; // Always null-terminate
}

// Validate pattern string (alphanumeric, dots, wildcards only)
static bool validatePattern(const char *pattern) {
    if (!pattern || strlen(pattern) == 0 ||
        strlen(pattern) >= MAX_PATTERN_LENGTH) {
        return false;
    }

    for (size_t i = 0; pattern[i] != '\0'; i++) {
        char c = pattern[i];
        if (!isalnum(c) && c != '.' && c != '*' && c != '#' && c != '_' &&
            c != '-') {
            return false;
        }
    }

    return true;
}

// Validate subscriber ID (non-zero, reasonable range)
static bool validateSubscriberId(uint32_t id) {
    return id > 0 && id < 0xFFFFFF; // Max 16 million subscribers
}

// Validate subscriber name
static bool validateSubscriberName(const char *name) {
    if (!name || strlen(name) == 0 || strlen(name) >= MAX_SUBSCRIBER_NAME) {
        return false;
    }

    for (size_t i = 0; name[i] != '\0'; i++) {
        if (!isalnum(name[i]) && name[i] != '_' && name[i] != '-') {
            return false;
        }
    }

    return true;
}

// ============================================================================
// SUBSCRIBER LIST OPERATIONS
// ============================================================================

static void subscriberListInit(SubscriberList *list) {
    list->count = 0;
}

static bool subscriberListAdd(SubscriberList *list, uint32_t id,
                              const char *name) {
    if (list->count >= MAX_SUBSCRIBERS) {
        return false;
    }

    // Check for duplicates
    for (size_t i = 0; i < list->count; i++) {
        if (list->subscribers[i].id == id) {
            return false; // Already exists
        }
    }

    list->subscribers[list->count].id = id;
    secureStrCopy(list->subscribers[list->count].name, MAX_SUBSCRIBER_NAME,
                  name);
    list->count++;
    return true;
}

static bool subscriberListRemove(SubscriberList *list, uint32_t id) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->subscribers[i].id == id) {
            // Shift remaining elements
            for (size_t j = i; j < list->count - 1; j++) {
                list->subscribers[j] = list->subscribers[j + 1];
            }
            list->count--;
            return true;
        }
    }
    return false;
}

static bool subscriberListContains(const SubscriberList *list, uint32_t id) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->subscribers[i].id == id) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// PATTERN PARSING
// ============================================================================

typedef struct {
    char segments[MAX_SEGMENTS][MAX_SEGMENT_LENGTH];
    SegmentType types[MAX_SEGMENTS];
    size_t count;
} ParsedPattern;

static bool parsePattern(const char *pattern, ParsedPattern *parsed) {
    if (!pattern || !parsed) {
        return false;
    }

    parsed->count = 0;
    const char *start = pattern;
    const char *end = pattern;

    while (*end != '\0' && parsed->count < MAX_SEGMENTS) {
        if (*end == '.') {
            size_t len = end - start;
            if (len == 0 || len >= MAX_SEGMENT_LENGTH) {
                return false;
            }

            if (len == 1 && *start == '*') {
                parsed->types[parsed->count] = SEGMENT_STAR;
                parsed->segments[parsed->count][0] = '*';
                parsed->segments[parsed->count][1] = '\0';
            } else if (len == 1 && *start == '#') {
                parsed->types[parsed->count] = SEGMENT_HASH;
                parsed->segments[parsed->count][0] = '#';
                parsed->segments[parsed->count][1] = '\0';
            } else {
                parsed->types[parsed->count] = SEGMENT_LITERAL;
                memcpy(parsed->segments[parsed->count], start, len);
                parsed->segments[parsed->count][len] = '\0';
            }

            parsed->count++;
            start = end + 1;
        }
        end++;
    }

    // Handle last segment
    if (start != end && parsed->count < MAX_SEGMENTS) {
        size_t len = end - start;
        if (len >= MAX_SEGMENT_LENGTH) {
            return false;
        }

        if (len == 1 && *start == '*') {
            parsed->types[parsed->count] = SEGMENT_STAR;
            parsed->segments[parsed->count][0] = '*';
            parsed->segments[parsed->count][1] = '\0';
        } else if (len == 1 && *start == '#') {
            parsed->types[parsed->count] = SEGMENT_HASH;
            parsed->segments[parsed->count][0] = '#';
            parsed->segments[parsed->count][1] = '\0';
        } else {
            parsed->types[parsed->count] = SEGMENT_LITERAL;
            memcpy(parsed->segments[parsed->count], start, len);
            parsed->segments[parsed->count][len] = '\0';
        }

        parsed->count++;
    }

    return parsed->count > 0;
}

// ============================================================================
// TRIE NODE OPERATIONS
// ============================================================================

static TrieNode *trieNodeCreate(const char *segment, SegmentType type) {
    TrieNode *node = (TrieNode *)calloc(1, sizeof(TrieNode));
    if (!node) {
        return NULL;
    }

    secureStrCopy(node->segment, MAX_SEGMENT_LENGTH, segment);
    node->type = type;
    node->isTerminal = false;
    subscriberListInit(&node->subscribers);
    node->children = NULL;
    node->childCount = 0;
    node->childCapacity = 0;

    return node;
}

static void trieNodeFree(TrieNode *node) {
    if (!node) {
        return;
    }

    for (size_t i = 0; i < node->childCount; i++) {
        trieNodeFree(node->children[i]);
    }
    free(node->children);
    free(node);
}

static bool trieNodeAddChild(TrieNode *parent, TrieNode *child) {
    if (!parent || !child) {
        return false;
    }

    if (parent->childCount >= parent->childCapacity) {
        size_t newCapacity =
            parent->childCapacity == 0 ? 4 : parent->childCapacity * 2;
        TrieNode **newChildren = (TrieNode **)realloc(
            parent->children, newCapacity * sizeof(TrieNode *));
        if (!newChildren) {
            return false;
        }

        parent->children = newChildren;
        parent->childCapacity = newCapacity;
    }

    parent->children[parent->childCount++] = child;
    return true;
}

static TrieNode *trieNodeFindChild(TrieNode *parent, const char *segment,
                                   SegmentType type) {
    if (!parent) {
        return NULL;
    }

    for (size_t i = 0; i < parent->childCount; i++) {
        TrieNode *child = parent->children[i];
        if (child->type == type && strcmp(child->segment, segment) == 0) {
            return child;
        }
    }

    return NULL;
}

void trieInit(PatternTrie *trie) {
    trie->root = trieNodeCreate("", SEGMENT_LITERAL);
    trie->patternCount = 0;
    trie->nodeCount = 1;
    trie->subscriberCount = 0;
}

void trieFree(PatternTrie *trie) {
    if (trie->root) {
        trieNodeFree(trie->root);
        trie->root = NULL;
    }
}

// ============================================================================
// TRIE OPERATIONS - Full implementations
// ============================================================================

static TrieNode *trieFindNode(TrieNode *root, const ParsedPattern *parsed) {
    if (!root || !parsed) {
        return NULL;
    }

    TrieNode *current = root;

    for (size_t i = 0; i < parsed->count; i++) {
        TrieNode *child =
            trieNodeFindChild(current, parsed->segments[i], parsed->types[i]);
        if (!child) {
            return NULL;
        }
        current = child;
    }

    return current;
}

bool trieInsert(PatternTrie *trie, const char *pattern, uint32_t subscriberId,
                const char *subscriberName) {
    if (!trie || !validatePattern(pattern) ||
        !validateSubscriberId(subscriberId) ||
        !validateSubscriberName(subscriberName)) {
        return false;
    }

    ParsedPattern parsed;
    if (!parsePattern(pattern, &parsed)) {
        return false;
    }

    TrieNode *current = trie->root;

    for (size_t i = 0; i < parsed.count; i++) {
        TrieNode *child =
            trieNodeFindChild(current, parsed.segments[i], parsed.types[i]);

        if (!child) {
            child = trieNodeCreate(parsed.segments[i], parsed.types[i]);
            if (!child) {
                return false;
            }

            if (!trieNodeAddChild(current, child)) {
                trieNodeFree(child);
                return false;
            }

            trie->nodeCount++;
        }

        current = child;
    }

    bool isNewPattern = !current->isTerminal;
    bool isNewSubscriber =
        !subscriberListContains(&current->subscribers, subscriberId);

    if (!subscriberListAdd(&current->subscribers, subscriberId,
                           subscriberName)) {
        return false;
    }

    current->isTerminal = true;

    if (isNewPattern) {
        trie->patternCount++;
    }
    if (isNewSubscriber) {
        trie->subscriberCount++;
    }

    return true;
}

bool trieRemovePattern(PatternTrie *trie, const char *pattern) {
    if (!trie || !validatePattern(pattern)) {
        return false;
    }

    ParsedPattern parsed;
    if (!parsePattern(pattern, &parsed)) {
        return false;
    }

    // Find the node
    TrieNode *node = trieFindNode(trie->root, &parsed);
    if (!node || !node->isTerminal) {
        return false; // Pattern doesn't exist
    }

    // Remove all subscribers and mark as non-terminal
    size_t removedSubscribers = node->subscribers.count;
    node->subscribers.count = 0;
    node->isTerminal = false;

    trie->patternCount--;
    trie->subscriberCount -= removedSubscribers;

    // TODO: Could implement node pruning here if node has no children
    // For now, we keep the structure (lazy deletion)

    return true;
}

bool trieRemoveSubscriber(PatternTrie *trie, const char *pattern,
                          uint32_t subscriberId) {
    if (!trie || !validatePattern(pattern) ||
        !validateSubscriberId(subscriberId)) {
        return false;
    }

    ParsedPattern parsed;
    if (!parsePattern(pattern, &parsed)) {
        return false;
    }

    TrieNode *node = trieFindNode(trie->root, &parsed);
    if (!node || !node->isTerminal) {
        return false;
    }

    if (!subscriberListRemove(&node->subscribers, subscriberId)) {
        return false;
    }

    trie->subscriberCount--;

    // If no more subscribers, mark as non-terminal
    if (node->subscribers.count == 0) {
        node->isTerminal = false;
        trie->patternCount--;
    }

    return true;
}

// ============================================================================
// PATTERN MATCHING
// ============================================================================

static void matchResultInit(MatchResult *result) {
    result->count = 0;
}

static void matchResultAdd(MatchResult *result,
                           const SubscriberList *subscribers) {
    for (size_t i = 0;
         i < subscribers->count && result->count < MAX_SUBSCRIBERS; i++) {
        // Check for duplicates
        bool exists = false;
        for (size_t j = 0; j < result->count; j++) {
            if (result->subscriberIds[j] == subscribers->subscribers[i].id) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            result->subscriberIds[result->count] =
                subscribers->subscribers[i].id;
            secureStrCopy(result->subscriberNames[result->count],
                          MAX_SUBSCRIBER_NAME,
                          subscribers->subscribers[i].name);
            result->count++;
        }
    }
}

static void trieMatchRecursive(TrieNode *node, const char **segments,
                               size_t segmentCount, size_t currentSegment,
                               MatchResult *result) {
    if (currentSegment >= segmentCount) {
        if (node->isTerminal) {
            matchResultAdd(result, &node->subscribers);
        }

        // Check for # wildcards that can match zero segments
        for (size_t i = 0; i < node->childCount; i++) {
            TrieNode *child = node->children[i];
            if (child->type == SEGMENT_HASH) {
                trieMatchRecursive(child, segments, segmentCount,
                                   currentSegment, result);
            }
        }
        return;
    }

    const char *segment = segments[currentSegment];

    for (size_t i = 0; i < node->childCount; i++) {
        TrieNode *child = node->children[i];

        if (child->type == SEGMENT_LITERAL) {
            if (strcmp(child->segment, segment) == 0) {
                trieMatchRecursive(child, segments, segmentCount,
                                   currentSegment + 1, result);
            }
        } else if (child->type == SEGMENT_STAR) {
            trieMatchRecursive(child, segments, segmentCount,
                               currentSegment + 1, result);
        } else if (child->type == SEGMENT_HASH) {
            // Try matching 0 segments
            trieMatchRecursive(child, segments, segmentCount, currentSegment,
                               result);
            // Try matching 1+ segments
            for (size_t j = currentSegment; j < segmentCount; j++) {
                trieMatchRecursive(child, segments, segmentCount, j + 1,
                                   result);
            }
        }
    }
}

void trieMatch(PatternTrie *trie, const char *input, MatchResult *result) {
    if (!trie || !input || !result) {
        return;
    }

    matchResultInit(result);

    ParsedPattern parsed;
    if (!parsePattern(input, &parsed)) {
        return;
    }

    const char *segments[MAX_SEGMENTS];
    for (size_t i = 0; i < parsed.count; i++) {
        segments[i] = parsed.segments[i];
    }

    trieMatchRecursive(trie->root, segments, parsed.count, 0, result);
}

// ============================================================================
// LISTING AND STATISTICS
// ============================================================================

static void trieListPatternsRecursive(TrieNode *node, char *currentPath,
                                      size_t pathLen,
                                      char patterns[][MAX_PATTERN_LENGTH],
                                      size_t *count, size_t maxCount) {
    if (!node || *count >= maxCount) {
        return;
    }

    if (node->isTerminal) {
        secureStrCopy(patterns[*count], MAX_PATTERN_LENGTH, currentPath);
        (*count)++;
    }

    for (size_t i = 0; i < node->childCount && *count < maxCount; i++) {
        TrieNode *child = node->children[i];

        size_t newLen = pathLen;
        if (pathLen > 0) {
            if (newLen + 1 < MAX_PATTERN_LENGTH) {
                currentPath[newLen++] = '.';
            }
        }

        size_t segLen = strlen(child->segment);
        if (newLen + segLen < MAX_PATTERN_LENGTH) {
            memcpy(currentPath + newLen, child->segment, segLen);
            currentPath[newLen + segLen] = '\0';

            trieListPatternsRecursive(child, currentPath, newLen + segLen,
                                      patterns, count, maxCount);
            currentPath[pathLen] = '\0'; // Restore path
        }
    }
}

void trieListPatterns(const PatternTrie *trie,
                      char patterns[][MAX_PATTERN_LENGTH], size_t *count,
                      size_t maxCount) {
    if (!trie || !patterns || !count) {
        return;
    }

    *count = 0;
    char currentPath[MAX_PATTERN_LENGTH] = "";

    trieListPatternsRecursive(trie->root, currentPath, 0, patterns, count,
                              maxCount);
}

void trieStats(const PatternTrie *trie, size_t *totalNodes,
               size_t *terminalNodes, size_t *wildcardNodes, size_t *maxDepth) {
    if (!trie) {
        return;
    }

    *totalNodes = 0;
    *terminalNodes = 0;
    *wildcardNodes = 0;
    *maxDepth = 0;

    // Allocate queue dynamically based on node count (with safety margin)
    size_t queueSize = (trie->nodeCount > 0) ? (trie->nodeCount + 100) : 4096;
    TrieNode **queue = (TrieNode **)malloc(queueSize * sizeof(TrieNode *));
    size_t *depths = (size_t *)malloc(queueSize * sizeof(size_t));

    if (!queue || !depths) {
        // Allocation failed, fall back to using trie metadata if available
        *totalNodes = trie->nodeCount;
        free(queue);
        free(depths);
        return;
    }

    size_t front = 0, back = 0;

    queue[back] = trie->root;
    depths[back] = 0;
    back++;

    while (front < back) {
        TrieNode *node = queue[front];
        size_t depth = depths[front];
        front++;

        (*totalNodes)++;
        if (node->isTerminal) {
            (*terminalNodes)++;
        }
        if (node->type != SEGMENT_LITERAL) {
            (*wildcardNodes)++;
        }
        if (depth > *maxDepth) {
            *maxDepth = depth;
        }

        for (size_t i = 0; i < node->childCount && back < queueSize; i++) {
            queue[back] = node->children[i];
            depths[back] = depth + 1;
            back++;
        }
    }

    free(queue);
    free(depths);
}

// ============================================================================
// PERSISTENCE (SERIALIZATION/DESERIALIZATION)
// ============================================================================

static size_t trieNodeSerialize(const TrieNode *node, uint8_t *buffer) {
    size_t offset = 0;

    // Node flags: isTerminal(1) | type(2) | reserved(5)
    uint64_t flags = 0;
    varintBitstreamSet(&flags, 0, 1, node->isTerminal ? 1 : 0);
    varintBitstreamSet(&flags, 1, 2, node->type);
    flags >>= 56;
    buffer[offset++] = (uint8_t)flags;

    // Segment length and data
    size_t segLen = strlen(node->segment);
    offset += varintTaggedPut64(buffer + offset, segLen);
    memcpy(buffer + offset, node->segment, segLen);
    offset += segLen;

    // Subscriber count and data
    offset += varintTaggedPut64(buffer + offset, node->subscribers.count);
    for (size_t i = 0; i < node->subscribers.count; i++) {
        offset += varintTaggedPut64(buffer + offset,
                                    node->subscribers.subscribers[i].id);

        size_t nameLen = strlen(node->subscribers.subscribers[i].name);
        offset += varintTaggedPut64(buffer + offset, nameLen);
        memcpy(buffer + offset, node->subscribers.subscribers[i].name, nameLen);
        offset += nameLen;
    }

    // Child count
    offset += varintTaggedPut64(buffer + offset, node->childCount);

    // Serialize children recursively
    for (size_t i = 0; i < node->childCount; i++) {
        offset += trieNodeSerialize(node->children[i], buffer + offset);
    }

    return offset;
}

static size_t trieNodeDeserialize(TrieNode **node, const uint8_t *buffer) {
    size_t offset = 0;

    *node = trieNodeCreate("", SEGMENT_LITERAL);
    if (!*node) {
        return 0;
    }

    // Read flags
    uint8_t flagsByte = buffer[offset++];
    uint64_t flags = (uint64_t)flagsByte << 56;
    (*node)->isTerminal = varintBitstreamGet(&flags, 0, 1) ? true : false;
    (*node)->type = (SegmentType)varintBitstreamGet(&flags, 1, 2);

    // Read segment
    uint64_t segLen;
    varintTaggedGet64(buffer + offset, &segLen);
    offset += varintTaggedGetLen(buffer + offset);

    if (segLen < MAX_SEGMENT_LENGTH) {
        memcpy((*node)->segment, buffer + offset, segLen);
        (*node)->segment[segLen] = '\0';
    }
    offset += segLen;

    // Read subscribers
    uint64_t subCount;
    varintTaggedGet64(buffer + offset, &subCount);
    offset += varintTaggedGetLen(buffer + offset);

    for (size_t i = 0; i < subCount && i < MAX_SUBSCRIBERS; i++) {
        uint64_t id;
        varintTaggedGet64(buffer + offset, &id);
        offset += varintTaggedGetLen(buffer + offset);

        uint64_t nameLen;
        varintTaggedGet64(buffer + offset, &nameLen);
        offset += varintTaggedGetLen(buffer + offset);

        char name[MAX_SUBSCRIBER_NAME];
        if (nameLen < MAX_SUBSCRIBER_NAME) {
            memcpy(name, buffer + offset, nameLen);
            name[nameLen] = '\0';
        } else {
            name[0] = '\0';
        }
        offset += nameLen;

        subscriberListAdd(&(*node)->subscribers, (uint32_t)id, name);
    }

    // Read children
    uint64_t childCount;
    varintTaggedGet64(buffer + offset, &childCount);
    offset += varintTaggedGetLen(buffer + offset);

    for (size_t i = 0; i < childCount; i++) {
        TrieNode *child;
        size_t childSize = trieNodeDeserialize(&child, buffer + offset);
        if (childSize == 0) {
            break;
        }

        trieNodeAddChild(*node, child);
        offset += childSize;
    }

    return offset;
}

bool trieSave(const PatternTrie *trie, const char *filename) {
    if (!trie || !filename) {
        return false;
    }

    FILE *file = fopen(filename, "wb");
    if (!file) {
        fprintf(stderr, "Error: Failed to open file for writing: %s (%s)\n",
                filename, strerror(errno));
        return false;
    }

    // Allocate buffer (max 16MB for safety)
    size_t bufferSize = 16 * 1024 * 1024;
    uint8_t *buffer = (uint8_t *)malloc(bufferSize);
    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate save buffer\n");
        fclose(file);
        return false;
    }

    size_t offset = 0;

    // Write magic header
    const char *magic = "TRIE";
    memcpy(buffer + offset, magic, 4);
    offset += 4;

    // Write version
    buffer[offset++] = 1;

    // Write metadata
    offset += varintTaggedPut64(buffer + offset, trie->patternCount);
    offset += varintTaggedPut64(buffer + offset, trie->nodeCount);
    offset += varintTaggedPut64(buffer + offset, trie->subscriberCount);

    // Serialize trie
    offset += trieNodeSerialize(trie->root, buffer + offset);

    // Write to file
    size_t written = fwrite(buffer, 1, offset, file);
    bool success = (written == offset);

    if (!success) {
        fprintf(stderr, "Error: Failed to write complete data to file\n");
    }

    free(buffer);
    fclose(file);

    return success;
}

bool trieLoad(PatternTrie *trie, const char *filename) {
    if (!trie || !filename) {
        return false;
    }

    FILE *file = fopen(filename, "rb");
    if (!file) {
        // Don't print error for missing file on initial load
        if (errno != ENOENT) {
            fprintf(stderr, "Error: Failed to open file for reading: %s (%s)\n",
                    filename, strerror(errno));
        }
        return false;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (fileSize <= 0 || fileSize > 16 * 1024 * 1024) {
        fprintf(stderr, "Error: Invalid file size: %" PRId64 " bytes\n",
                (int64_t)fileSize);
        fclose(file);
        return false;
    }

    uint8_t *buffer = (uint8_t *)malloc(fileSize);
    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate load buffer\n");
        fclose(file);
        return false;
    }

    size_t readSize = fread(buffer, 1, fileSize, file);
    fclose(file);

    if (readSize != (size_t)fileSize) {
        fprintf(stderr, "Error: Failed to read complete file\n");
        free(buffer);
        return false;
    }

    size_t offset = 0;

    // Read and verify magic header
    if (memcmp(buffer + offset, "TRIE", 4) != 0) {
        fprintf(stderr, "Error: Invalid file format (bad magic header)\n");
        free(buffer);
        return false;
    }
    offset += 4;

    // Read version
    uint8_t version = buffer[offset++];
    if (version != 1) {
        fprintf(stderr, "Error: Unsupported file version: %u\n", version);
        free(buffer);
        return false;
    }

    // Read metadata
    uint64_t patternCount, nodeCount, subscriberCount;
    varintTaggedGet64(buffer + offset, &patternCount);
    offset += varintTaggedGetLen(buffer + offset);
    varintTaggedGet64(buffer + offset, &nodeCount);
    offset += varintTaggedGetLen(buffer + offset);
    varintTaggedGet64(buffer + offset, &subscriberCount);
    offset += varintTaggedGetLen(buffer + offset);

    // Clear existing trie (preserve root structure but reset it)
    trieNodeFree(trie->root);
    trie->root = trieNodeCreate("", SEGMENT_LITERAL);
    if (!trie->root) {
        fprintf(stderr, "Error: Failed to create root node\n");
        free(buffer);
        return false;
    }

    // Deserialize root node
    TrieNode *loadedRoot;
    size_t deserializedSize = trieNodeDeserialize(&loadedRoot, buffer + offset);
    if (deserializedSize == 0) {
        fprintf(stderr, "Error: Failed to deserialize trie structure\n");
        free(buffer);
        return false;
    }

    // Copy loaded root's data to existing root
    trieNodeFree(trie->root);
    trie->root = loadedRoot;

    // Update trie metadata
    trie->patternCount = patternCount;
    trie->nodeCount = nodeCount;
    trie->subscriberCount = subscriberCount;

    free(buffer);
    return true;
}

// ============================================================================
// CLIENT MANAGEMENT IMPLEMENTATION
// ============================================================================

// Simple hash function for 64-bit keys
static size_t hashKey(uint64_t key, size_t bucketCount) {
    // MurmurHash-inspired mixing
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return (size_t)(key % bucketCount);
}

// Initialize hash table
static bool hashTableInit(ClientHashTable *ht, size_t bucketCount) {
    ht->buckets =
        (ClientMapEntry **)calloc(bucketCount, sizeof(ClientMapEntry *));
    if (!ht->buckets) {
        return false;
    }
    ht->bucketCount = bucketCount;
    ht->itemCount = 0;
    return true;
}

// Destroy hash table
static void hashTableDestroy(ClientHashTable *ht) {
    if (!ht->buckets) {
        return;
    }

    for (size_t i = 0; i < ht->bucketCount; i++) {
        ClientMapEntry *entry = ht->buckets[i];
        while (entry) {
            ClientMapEntry *next = entry->next;
            free(entry);
            entry = next;
        }
    }

    free(ht->buckets);
    ht->buckets = NULL;
    ht->bucketCount = 0;
    ht->itemCount = 0;
}

// Insert into hash table
static bool hashTableInsert(ClientHashTable *ht, uint64_t key,
                            ClientConnection *conn) {
    size_t bucket = hashKey(key, ht->bucketCount);

    // Check if already exists
    ClientMapEntry *entry = ht->buckets[bucket];
    while (entry) {
        if (entry->key == key) {
            entry->conn = conn; // Update
            return true;
        }
        entry = entry->next;
    }

    // Create new entry
    ClientMapEntry *newEntry = (ClientMapEntry *)malloc(sizeof(ClientMapEntry));
    if (!newEntry) {
        return false;
    }

    newEntry->key = key;
    newEntry->conn = conn;
    newEntry->next = ht->buckets[bucket];
    ht->buckets[bucket] = newEntry;
    ht->itemCount++;

    return true;
}

// Lookup in hash table
static ClientConnection *hashTableGet(ClientHashTable *ht, uint64_t key) {
    if (!ht->buckets) {
        return NULL;
    }

    size_t bucket = hashKey(key, ht->bucketCount);
    ClientMapEntry *entry = ht->buckets[bucket];

    while (entry) {
        if (entry->key == key) {
            return entry->conn;
        }
        entry = entry->next;
    }

    return NULL;
}

// Remove from hash table
static bool hashTableRemove(ClientHashTable *ht, uint64_t key) {
    if (!ht->buckets) {
        return false;
    }

    size_t bucket = hashKey(key, ht->bucketCount);
    ClientMapEntry **entryPtr = &ht->buckets[bucket];

    while (*entryPtr) {
        ClientMapEntry *entry = *entryPtr;
        if (entry->key == key) {
            *entryPtr = entry->next;
            free(entry);
            ht->itemCount--;
            return true;
        }
        entryPtr = &entry->next;
    }

    return false;
}

// Initialize client manager
bool clientMgrInit(ClientManager *mgr, size_t initialCapacity) {
    memset(mgr, 0, sizeof(ClientManager));

    // Initialize hash tables with prime bucket counts for better distribution
    if (!hashTableInit(&mgr->byId, 1009)) { // ~1000 buckets
        return false;
    }

    if (!hashTableInit(&mgr->byFd, 1009)) {
        hashTableDestroy(&mgr->byId);
        return false;
    }

    // Initialize active list
    mgr->activeCapacity = initialCapacity > 0 ? initialCapacity : 128;
    mgr->activeList = (ClientConnection **)calloc(mgr->activeCapacity,
                                                  sizeof(ClientConnection *));
    if (!mgr->activeList) {
        hashTableDestroy(&mgr->byId);
        hashTableDestroy(&mgr->byFd);
        return false;
    }
    mgr->activeCount = 0;

    // Initialize client pool
    mgr->poolCapacity = initialCapacity > 0 ? initialCapacity : 128;
    mgr->allocatedPool =
        (ClientConnection *)calloc(mgr->poolCapacity, sizeof(ClientConnection));
    if (!mgr->allocatedPool) {
        free(mgr->activeList);
        hashTableDestroy(&mgr->byId);
        hashTableDestroy(&mgr->byFd);
        return false;
    }

    // Build free list
    mgr->freeList = NULL;
    for (size_t i = 0; i < mgr->poolCapacity; i++) {
        ClientConnection *conn = &mgr->allocatedPool[i];
        conn->fd = -1;
        conn->state = CONN_CLOSED;
        // Link into free list (using writeBuffer as next pointer for free list)
        *(ClientConnection **)conn->writeBuffer = mgr->freeList;
        mgr->freeList = conn;
    }
    mgr->poolSize = 0;

    return true;
}

// Destroy client manager
void clientMgrDestroy(ClientManager *mgr) {
    // Clean up all active clients
    for (size_t i = 0; i < mgr->activeCount; i++) {
        if (mgr->activeList[i]) {
            cleanupClientPubSub(mgr->activeList[i]);
        }
    }

    // Free data structures
    hashTableDestroy(&mgr->byId);
    hashTableDestroy(&mgr->byFd);
    free(mgr->activeList);
    free(mgr->allocatedPool);

    memset(mgr, 0, sizeof(ClientManager));
}

// Allocate a client connection
ClientConnection *clientMgrAllocate(ClientManager *mgr, int fd,
                                    uint64_t clientId) {
    ClientConnection *conn = NULL;

    // Try to get from free list
    if (mgr->freeList) {
        conn = mgr->freeList;
        mgr->freeList = *(ClientConnection **)conn->writeBuffer;
        mgr->poolSize++;
    } else {
        // Need to expand pool
        size_t newCapacity = mgr->poolCapacity * 2;
        ClientConnection *newPool = (ClientConnection *)realloc(
            mgr->allocatedPool, newCapacity * sizeof(ClientConnection));

        if (!newPool) {
            return NULL; // Out of memory
        }

        mgr->allocatedPool = newPool;

        // Initialize new entries and add to free list
        for (size_t i = mgr->poolCapacity; i < newCapacity; i++) {
            ClientConnection *c = &mgr->allocatedPool[i];
            c->fd = -1;
            c->state = CONN_CLOSED;
            *(ClientConnection **)c->writeBuffer = mgr->freeList;
            mgr->freeList = c;
        }

        mgr->poolCapacity = newCapacity;

        // Now allocate from free list
        conn = mgr->freeList;
        mgr->freeList = *(ClientConnection **)conn->writeBuffer;
        mgr->poolSize++;
    }

    // Initialize the connection
    memset(conn, 0, sizeof(ClientConnection));
    conn->fd = fd;
    conn->clientId = clientId;
    conn->state = CONN_READING_LENGTH;
    initClientPubSub(conn, clientId);

    return conn;
}

// Free a client connection
void clientMgrFree(ClientManager *mgr, ClientConnection *conn) {
    if (!conn) {
        return;
    }

    // Cleanup pub/sub resources
    cleanupClientPubSub(conn);

    // Reset connection
    memset(conn, 0, sizeof(ClientConnection));
    conn->fd = -1;
    conn->state = CONN_CLOSED;

    // Add back to free list
    *(ClientConnection **)conn->writeBuffer = mgr->freeList;
    mgr->freeList = conn;
    mgr->poolSize--;
}

// Add client to active set
bool clientMgrAdd(ClientManager *mgr, ClientConnection *conn) {
    if (!conn) {
        return false;
    }

    // Add to hash tables
    if (!hashTableInsert(&mgr->byId, conn->clientId, conn)) {
        return false;
    }

    if (!hashTableInsert(&mgr->byFd, (uint64_t)conn->fd, conn)) {
        hashTableRemove(&mgr->byId, conn->clientId);
        return false;
    }

    // Add to active list
    if (mgr->activeCount >= mgr->activeCapacity) {
        size_t newCapacity = mgr->activeCapacity * 2;
        ClientConnection **newList = (ClientConnection **)realloc(
            mgr->activeList, newCapacity * sizeof(ClientConnection *));

        if (!newList) {
            hashTableRemove(&mgr->byId, conn->clientId);
            hashTableRemove(&mgr->byFd, (uint64_t)conn->fd);
            return false;
        }

        mgr->activeList = newList;
        mgr->activeCapacity = newCapacity;
    }

    mgr->activeList[mgr->activeCount++] = conn;
    return true;
}

// Remove client from active set
void clientMgrRemove(ClientManager *mgr, const ClientConnection *conn) {
    if (!conn) {
        return;
    }

    // Remove from hash tables
    hashTableRemove(&mgr->byId, conn->clientId);
    hashTableRemove(&mgr->byFd, (uint64_t)conn->fd);

    // Remove from active list
    for (size_t i = 0; i < mgr->activeCount; i++) {
        if (mgr->activeList[i] == conn) {
            // Swap with last element and shrink
            mgr->activeList[i] = mgr->activeList[mgr->activeCount - 1];
            mgr->activeCount--;
            break;
        }
    }
}

// Get client by ID
ClientConnection *clientMgrGetById(ClientManager *mgr, uint64_t clientId) {
    return hashTableGet(&mgr->byId, clientId);
}

// Get client by FD
ClientConnection *clientMgrGetByFd(ClientManager *mgr, int fd) {
    return hashTableGet(&mgr->byFd, (uint64_t)fd);
}

// Get active client count
size_t clientMgrGetActiveCount(const ClientManager *mgr) {
    return mgr->activeCount;
}

// ============================================================================
// MESSAGE POOL IMPLEMENTATION (PHASE 3)
// ============================================================================

// Initialize message pool with pre-allocated BufferedMessage structures
bool msgPoolInit(MessagePool *pool, size_t capacity) {
    memset(pool, 0, sizeof(MessagePool));

    pool->capacity = capacity;
    pool->messages = calloc(capacity, sizeof(BufferedMessage));
    pool->inUse = calloc(capacity, sizeof(bool));
    pool->freeList = malloc(capacity * sizeof(size_t));

    if (!pool->messages || !pool->inUse || !pool->freeList) {
        free(pool->messages);
        free(pool->inUse);
        free(pool->freeList);
        return false;
    }

    // Initialize free list - all messages available
    pool->freeCount = capacity;
    for (size_t i = 0; i < capacity; i++) {
        pool->freeList[i] = i;
        pool->inUse[i] = false;
    }

    return true;
}

// Destroy message pool
void msgPoolDestroy(MessagePool *pool) {
    if (!pool) {
        return;
    }

    // Free any payloads still allocated
    for (size_t i = 0; i < pool->capacity; i++) {
        if (pool->inUse[i] && pool->messages[i].payload) {
            free(pool->messages[i].payload);
        }
    }

    free(pool->messages);
    free(pool->inUse);
    free(pool->freeList);
    memset(pool, 0, sizeof(MessagePool));
}

// Allocate a BufferedMessage from the pool - O(1)
BufferedMessage *msgPoolAlloc(MessagePool *pool) {
    if (!pool || pool->freeCount == 0) {
        return NULL; // Pool exhausted
    }

    // Pop from free list
    size_t idx = pool->freeList[--pool->freeCount];
    pool->inUse[idx] = true;

    BufferedMessage *msg = &pool->messages[idx];
    memset(msg, 0, sizeof(BufferedMessage));

    return msg;
}

// Return a BufferedMessage to the pool - O(1)
void msgPoolFree(MessagePool *pool, BufferedMessage *msg) {
    if (!pool || !msg) {
        return;
    }

    // Find index of message in pool
    size_t idx = msg - pool->messages;
    if (idx >= pool->capacity || !pool->inUse[idx]) {
        return; // Invalid message or already freed
    }

    // Free payload if allocated (not from buffer pool)
    if (msg->payload) {
        free(msg->payload);
        msg->payload = NULL;
    }

    // Free pending client list if allocated
    if (msg->pendingClientFds) {
        free(msg->pendingClientFds);
        msg->pendingClientFds = NULL;
    }

    // Clear and return to free list
    memset(msg, 0, sizeof(BufferedMessage));
    pool->inUse[idx] = false;
    pool->freeList[pool->freeCount++] = idx;
}

// ============================================================================
// BUFFER POOL IMPLEMENTATION (PHASE 3)
// ============================================================================

// Initialize a single buffer tier with dynamic expansion
static bool bufferTierInit(BufferTier *tier, size_t bufferSize,
                           size_t initialCapacity) {
    memset(tier, 0, sizeof(BufferTier));

    tier->bufferSize = bufferSize;
    tier->capacity = initialCapacity;
    tier->initialCapacity = initialCapacity;
    tier->buffers = calloc(initialCapacity, sizeof(PooledBuffer));
    tier->freeList = malloc(initialCapacity * sizeof(size_t));
    tier->totalAllocated = 0;
    tier->expansionCount = 0;

    if (!tier->buffers || !tier->freeList) {
        free(tier->buffers);
        free(tier->freeList);
        return false;
    }

    // Pre-allocate all buffers
    for (size_t i = 0; i < initialCapacity; i++) {
        tier->buffers[i].data = malloc(bufferSize);
        if (!tier->buffers[i].data) {
            // Cleanup on failure
            for (size_t j = 0; j < i; j++) {
                free(tier->buffers[j].data);
            }
            free(tier->buffers);
            free(tier->freeList);
            return false;
        }
        tier->buffers[i].size = bufferSize;
        tier->buffers[i].inUse = false;
        tier->freeList[i] = i;
    }

    tier->freeCount = initialCapacity;
    return true;
}

// Expand buffer tier when exhausted - doubles capacity
static bool bufferTierExpand(BufferTier *tier) {
    size_t oldCapacity = tier->capacity;
    size_t newCapacity = oldCapacity * 2; // Double capacity

    // Reallocate buffers array
    PooledBuffer *newBuffers =
        realloc(tier->buffers, newCapacity * sizeof(PooledBuffer));
    if (!newBuffers) {
        return false;
    }
    tier->buffers = newBuffers;

    // Reallocate free list
    size_t *newFreeList = realloc(tier->freeList, newCapacity * sizeof(size_t));
    if (!newFreeList) {
        return false;
    }
    tier->freeList = newFreeList;

    // Allocate new buffers for the expanded portion
    for (size_t i = oldCapacity; i < newCapacity; i++) {
        tier->buffers[i].data = malloc(tier->bufferSize);
        if (!tier->buffers[i].data) {
            // Cleanup newly allocated buffers on failure
            for (size_t j = oldCapacity; j < i; j++) {
                free(tier->buffers[j].data);
            }
            return false;
        }
        tier->buffers[i].size = tier->bufferSize;
        tier->buffers[i].inUse = false;
        // Add to free list
        tier->freeList[tier->freeCount++] = i;
    }

    tier->capacity = newCapacity;
    tier->expansionCount++;

    DEBUG_LOG("Expanded buffer tier (size=%zu) from %zu to %zu buffers "
              "(expansion #%zu)\n",
              tier->bufferSize, oldCapacity, newCapacity, tier->expansionCount);

    return true;
}

// Destroy a single buffer tier
static void bufferTierDestroy(BufferTier *tier) {
    if (!tier) {
        return;
    }

    if (tier->buffers) {
        for (size_t i = 0; i < tier->capacity; i++) {
            free(tier->buffers[i].data);
        }
        free(tier->buffers);
    }

    free(tier->freeList);
    memset(tier, 0, sizeof(BufferTier));
}

// Initialize buffer pool - pools SMALL common sizes, uses malloc for
// large/arbitrary NO SIZE RESTRICTIONS - pool is just an optimization for
// common small payloads
bool bufferPoolInit(BufferPoolManager *mgr) {
    memset(mgr, 0, sizeof(BufferPoolManager));

    // Pool small common sizes - beyond this, use malloc (supports ANY size)
    // These tiers are optimizations for 90%+ of typical messages
    const size_t tierSizes[] = {256,  512,   1024,  2048, 4096,
                                8192, 16384, 32768, 65536};
    const size_t initialCapacities[] = {16, 12, 10, 8, 6, 4, 3, 2, 2};

    mgr->tierCount = sizeof(tierSizes) / sizeof(tierSizes[0]);
    mgr->maxPooledSize = tierSizes[mgr->tierCount - 1]; // 64KB
    mgr->tiers = calloc(mgr->tierCount, sizeof(BufferTier));

    if (!mgr->tiers) {
        return false;
    }

    for (size_t i = 0; i < mgr->tierCount; i++) {
        if (!bufferTierInit(&mgr->tiers[i], tierSizes[i],
                            initialCapacities[i])) {
            // Cleanup on failure
            for (size_t j = 0; j < i; j++) {
                bufferTierDestroy(&mgr->tiers[j]);
            }
            free(mgr->tiers);
            return false;
        }
    }

    DEBUG_LOG("Buffer pool initialized: %zu tiers (256B-64KB), malloc for "
              "larger sizes\n",
              mgr->tierCount);
    DEBUG_LOG("  -> NO SIZE LIMIT: Pool optimizes small msgs, malloc handles "
              "1MB/1GB/any size\n");
    return true;
}

// Destroy buffer pool
void bufferPoolDestroy(BufferPoolManager *mgr) {
    if (!mgr) {
        return;
    }

    if (mgr->tiers) {
        for (size_t i = 0; i < mgr->tierCount; i++) {
            bufferTierDestroy(&mgr->tiers[i]);
        }
        free(mgr->tiers);
    }

    DEBUG_LOG("Buffer pool stats: %zu allocs, %zu from pool, %zu via malloc "
              "(%zu bytes direct)\n",
              mgr->totalAllocations, mgr->poolHits, mgr->poolMisses,
              mgr->directAllocBytes);

    memset(mgr, 0, sizeof(BufferPoolManager));
}

// Allocate buffer - uses pool for small sizes, malloc for large/arbitrary sizes
// SUPPORTS ANY SIZE from 1 byte to gigabytes - NO RESTRICTIONS
uint8_t *bufferPoolAlloc(BufferPoolManager *mgr, size_t size) {
    if (!mgr) {
        return NULL;
    }

    mgr->totalAllocations++;

    // For sizes beyond largest pooled tier, use malloc directly
    // This supports 1MB, 1GB, or any arbitrary size without restriction
    if (size > mgr->maxPooledSize) {
        mgr->poolMisses++;
        mgr->directAllocBytes += size;
        return malloc(size);
    }

    // Find smallest tier that fits this size - O(log n) with binary search
    // or O(n) with linear search (fine for small tierCount)
    int tierIdx = -1;
    for (size_t i = 0; i < mgr->tierCount; i++) {
        if (size <= mgr->tiers[i].bufferSize) {
            tierIdx = (int)i;
            break;
        }
    }

    if (tierIdx < 0) {
        // Should not happen (size <= maxPooledSize but no tier found)
        mgr->poolMisses++;
        mgr->directAllocBytes += size;
        return malloc(size);
    }

    BufferTier *tier = &mgr->tiers[tierIdx];

    // Check if tier has available buffers - expand if exhausted
    if (tier->freeCount == 0) {
        // Tier exhausted - try to expand it dynamically
        if (!bufferTierExpand(tier)) {
            // Expansion failed (out of memory) - fallback to malloc
            mgr->poolMisses++;
            mgr->directAllocBytes += size;
            DEBUG_LOG("Buffer pool tier %d exhausted and expansion failed, "
                      "using malloc\n",
                      tierIdx);
            return malloc(size);
        }
        // Expansion succeeded! Continue with allocation from expanded pool
    }

    // Pop from free list
    size_t idx = tier->freeList[--tier->freeCount];
    tier->buffers[idx].inUse = true;
    tier->totalAllocated++;

    mgr->poolHits++;
    return tier->buffers[idx].data;
}

// Free buffer back to pool - O(1)
void bufferPoolFree(BufferPoolManager *mgr, uint8_t *buffer, size_t size) {
    if (!mgr || !buffer) {
        return;
    }

    mgr->totalFrees++;

    // For sizes beyond largest pooled tier, it was malloc'd - free it
    if (size > mgr->maxPooledSize) {
        free(buffer);
        return;
    }

    // Find which tier this buffer belongs to based on size
    int tierIdx = -1;
    for (size_t i = 0; i < mgr->tierCount; i++) {
        if (size <= mgr->tiers[i].bufferSize) {
            tierIdx = (int)i;
            break;
        }
    }

    if (tierIdx < 0) {
        // Should not happen, but fallback to free
        free(buffer);
        return;
    }

    BufferTier *tier = &mgr->tiers[tierIdx];

    // Find buffer index in tier
    for (size_t i = 0; i < tier->capacity; i++) {
        if (tier->buffers[i].data == buffer) {
            if (!tier->buffers[i].inUse) {
                // Already freed - double free bug!
                return;
            }

            // Return to free list
            tier->buffers[i].inUse = false;
            tier->freeList[tier->freeCount++] = i;
            return;
        }
    }

    // Buffer not found in pool - must have been malloc'd
    free(buffer);
}

// ============================================================================
// PUB/SUB IMPLEMENTATION
// ============================================================================

void initClientPubSub(ClientConnection *client, uint64_t clientId) {
    client->subscriptionCount = 0;
    client->messageQueue = NULL;
    client->queueSize = 0;
    client->queueCapacity = 0;
    client->nextSeqNum = 1;
    client->defaultQos = QOS_AT_MOST_ONCE;
    client->clientId = clientId;
    client->hasIdentity = false;
    client->clientName[0] = '\0';
    client->pendingNotifications = NULL;
    client->pendingNotificationCount = 0;
    client->pendingNotificationCapacity = 0;
}

void cleanupClientPubSub(ClientConnection *client) {
    // Free message queue
    if (client->messageQueue) {
        for (size_t i = 0; i < client->queueSize; i++) {
            free(client->messageQueue[i].payload);
            free(client->messageQueue[i].pendingClientFds);
        }
        free(client->messageQueue);
        client->messageQueue = NULL;
    }
    client->queueSize = 0;
    client->queueCapacity = 0;

    // Free pending notifications
    if (client->pendingNotifications) {
        free(client->pendingNotifications);
        client->pendingNotifications = NULL;
    }
    client->pendingNotificationCount = 0;
    client->pendingNotificationCapacity = 0;

    client->subscriptionCount = 0;
}

bool addClientSubscription(ClientConnection *client, const char *pattern,
                           QoSLevel qos) {
    if (!client || !pattern) {
        return false;
    }

    // Check if already subscribed
    for (size_t i = 0; i < client->subscriptionCount; i++) {
        if (strcmp(client->subscriptions[i].pattern, pattern) == 0) {
            // Update QoS if already subscribed
            client->subscriptions[i].qos = qos;
            client->subscriptions[i].active = true;
            return true;
        }
    }

    // Add new subscription
    if (client->subscriptionCount >= MAX_SUBSCRIPTIONS_PER_CLIENT) {
        return false;
    }

    ConnectionSubscription *sub =
        &client->subscriptions[client->subscriptionCount];
    secureStrCopy(sub->pattern, MAX_PATTERN_LENGTH, pattern);
    sub->qos = qos;
    sub->lastSeqNum = 0;
    sub->active = true;
    client->subscriptionCount++;

    return true;
}

bool removeClientSubscription(ClientConnection *client, const char *pattern) {
    if (!client || !pattern) {
        return false;
    }

    for (size_t i = 0; i < client->subscriptionCount; i++) {
        if (strcmp(client->subscriptions[i].pattern, pattern) == 0) {
            // Mark as inactive rather than removing to preserve indices
            client->subscriptions[i].active = false;
            return true;
        }
    }

    return false;
}

bool publishMessage(TrieServer *server, const char *pattern,
                    const uint8_t *payload, size_t payloadLen,
                    uint64_t publisherId, const char *publisherName) {
    if (!server || !pattern || !validatePattern(pattern)) {
        return false;
    }

    if (payloadLen > MAX_PAYLOAD_SIZE) {
        return false;
    }

    DEBUG_LOG("Publishing message to pattern '%s' with %zu bytes payload\n",
              pattern, payloadLen);

    // Match pattern to find subscribers (trie handles wildcards!)
    MatchResult result;
    trieMatch(&server->trie, pattern, &result);

    DEBUG_LOG("Pattern matched %zu subscribers in trie\n", result.count);

    // Phase 2 Optimization: Use trie results + O(1) client lookups
    // The trie returns subscriber IDs which ARE client IDs for live
    // subscriptions Use clientMgrGetById for direct O(1) lookup instead of
    // iterating all clients!

    ClientConnection *matchedClients[1024];
    size_t matchedCount = 0;

    for (size_t i = 0; i < result.count; i++) {
        uint64_t clientId = result.subscriberIds[i];

        // O(1) hash table lookup by client ID
        ClientConnection *client =
            clientMgrGetById(&server->clientMgr, clientId);

        if (client && client->fd >= 0 && client->authenticated) {
            matchedClients[matchedCount++] = client;
        }
    }

    DEBUG_LOG("Found %zu active clients to notify (was O(n×m), now O(k) where "
              "k=matched)\n",
              matchedCount);

    if (matchedCount == 0) {
        // No active subscribers
        return true;
    }

    // Create buffered message
    BufferedMessage msg;
    msg.seqNum = server->nextGlobalSeqNum++;
    msg.timestamp = time(NULL);
    secureStrCopy(msg.pattern, MAX_PATTERN_LENGTH, pattern);

    // Phase 3: Use buffer pool instead of malloc for payload
    msg.payload = bufferPoolAlloc(&server->bufferPool, payloadLen);
    if (!msg.payload) {
        return false;
    }
    memcpy(msg.payload, payload, payloadLen);
    msg.payloadLen = payloadLen;
    msg.publisherId = publisherId;
    secureStrCopy(msg.publisherName, MAX_SUBSCRIBER_NAME, publisherName);

    // Track which clients need to acknowledge (for QoS=1)
    msg.pendingClientFds = (int *)malloc(matchedCount * sizeof(int));
    if (!msg.pendingClientFds) {
        // Phase 3: Use buffer pool free
        bufferPoolFree(&server->bufferPool, msg.payload, payloadLen);
        return false;
    }
    msg.pendingClientCount = 0;

    // Send to all matched clients
    for (size_t i = 0; i < matchedCount; i++) {
        ClientConnection *client =
            matchedClients[i]; // Now using pointers directly

        // Determine QoS for this client
        QoSLevel qos = QOS_AT_MOST_ONCE;
        for (size_t j = 0; j < client->subscriptionCount; j++) {
            if (client->subscriptions[j].active) {
                qos = client->subscriptions[j].qos;
                break; // Use first active subscription's QoS
            }
        }

        if (qos == QOS_AT_LEAST_ONCE) {
            msg.pendingClientFds[msg.pendingClientCount++] = client->fd;
        }

        // Send notification immediately
        sendNotification(server, client, &msg);
        server->totalNotificationsSent++;
    }

    // Store message in global buffer if any clients need QoS=1
    if (msg.pendingClientCount > 0) {
        if (server->globalBufferSize >= server->globalBufferCapacity) {
            size_t newCapacity = server->globalBufferCapacity == 0
                                     ? 1000
                                     : server->globalBufferCapacity * 2;
            BufferedMessage *newBuffer = (BufferedMessage *)realloc(
                server->globalMessageBuffer,
                newCapacity * sizeof(BufferedMessage));
            if (!newBuffer) {
                // Phase 3: Use buffer pool free
                bufferPoolFree(&server->bufferPool, msg.payload,
                               msg.payloadLen);
                free(msg.pendingClientFds);
                return false;
            }
            server->globalMessageBuffer = newBuffer;
            server->globalBufferCapacity = newCapacity;
        }

        server->globalMessageBuffer[server->globalBufferSize++] = msg;
    } else {
        // No QoS=1 clients, free the message immediately
        // Phase 3: Use buffer pool free
        bufferPoolFree(&server->bufferPool, msg.payload, msg.payloadLen);
        free(msg.pendingClientFds);
    }

    server->totalPublishes++;
    return true;
}

void sendNotification(TrieServer *server, ClientConnection *client,
                      const BufferedMessage *msg) {
    (void)server; // Unused parameter
    DEBUG_LOG("Sending notification to client fd=%d for pattern '%s'\n",
              client->fd, msg->pattern);

    // Build notification message:
    // [Length:varint][MSG_NOTIFICATION:1byte][SeqNum:varint][Pattern:string][PublisherId:varint][PublisherName:string][Payload:bytes]
    uint8_t buffer[MAX_MESSAGE_SIZE];
    size_t offset = 5; // Reserve space for length

    // Message type
    buffer[offset++] = MSG_NOTIFICATION;

    // Sequence number
    offset += varintTaggedPut64(buffer + offset, msg->seqNum);

    // Pattern
    size_t patternLen = strlen(msg->pattern);
    offset += varintTaggedPut64(buffer + offset, patternLen);
    memcpy(buffer + offset, msg->pattern, patternLen);
    offset += patternLen;

    // Publisher ID
    offset += varintTaggedPut64(buffer + offset, msg->publisherId);

    // Publisher name
    size_t nameLen = strlen(msg->publisherName);
    offset += varintTaggedPut64(buffer + offset, nameLen);
    memcpy(buffer + offset, msg->publisherName, nameLen);
    offset += nameLen;

    // Payload length and data
    offset += varintTaggedPut64(buffer + offset, msg->payloadLen);
    if (offset + msg->payloadLen > sizeof(buffer)) {
        DEBUG_LOG("Notification too large, truncating\n");
        return; // Message too large
    }
    memcpy(buffer + offset, msg->payload, msg->payloadLen);
    offset += msg->payloadLen;

    // Calculate total length
    uint64_t messageLen = offset - 5;
    size_t lengthBytes = varintTaggedPut64(buffer, messageLen);

    // Shift message to remove extra length padding
    memmove(buffer + lengthBytes, buffer + 5, messageLen);
    size_t totalSize = lengthBytes + messageLen;

    // Try to send immediately (non-blocking)
    ssize_t sent = write(client->fd, buffer, totalSize);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            DEBUG_LOG("Client fd=%d would block, queueing notification\n",
                      client->fd);
            // TODO: Queue for later delivery
        } else {
            DEBUG_LOG("Failed to send notification to fd=%d: %s\n", client->fd,
                      strerror(errno));
        }
    } else if ((size_t)sent < totalSize) {
        DEBUG_LOG("Partial send to fd=%d: %zd/%zu bytes\n", client->fd, sent,
                  totalSize);
        // TODO: Queue remainder for later delivery
    } else {
        DEBUG_LOG("Notification sent successfully to fd=%d (%zu bytes)\n",
                  client->fd, totalSize);
    }
}

void queueNotificationForClient(TrieServer *server, ClientConnection *client,
                                size_t msgIndex) {
    (void)server;
    if (!client) {
        return;
    }

    // Grow pending notifications array if needed
    if (client->pendingNotificationCount >=
        client->pendingNotificationCapacity) {
        size_t newCapacity = client->pendingNotificationCapacity == 0
                                 ? 10
                                 : client->pendingNotificationCapacity * 2;
        size_t *newArray = (size_t *)realloc(client->pendingNotifications,
                                             newCapacity * sizeof(size_t));
        if (!newArray) {
            return;
        }
        client->pendingNotifications = newArray;
        client->pendingNotificationCapacity = newCapacity;
    }

    client->pendingNotifications[client->pendingNotificationCount++] = msgIndex;
}

void processNotificationQueue(TrieServer *server, ClientConnection *client) {
    if (!client || client->pendingNotificationCount == 0) {
        return;
    }

    DEBUG_LOG("Processing %zu pending notifications for fd=%d\n",
              client->pendingNotificationCount, client->fd);

    // Try to send all pending notifications
    for (size_t i = 0; i < client->pendingNotificationCount; i++) {
        size_t msgIndex = client->pendingNotifications[i];
        if (msgIndex >= server->globalBufferSize) {
            continue; // Message was already cleaned up
        }

        const BufferedMessage *msg = &server->globalMessageBuffer[msgIndex];
        sendNotification(server, client, msg);
    }

    DEBUG_LOG("Sent %zu pending notifications to client fd=%d\n",
              client->pendingNotificationCount, client->fd);

    // Clear processed notifications
    client->pendingNotificationCount = 0;
}

void acknowledgeMessage(TrieServer *server, ClientConnection *client,
                        uint64_t seqNum) {
    DEBUG_LOG("Client fd=%d acknowledging message seqNum=%" PRIu64 "\n",
              client->fd, seqNum);

    // Find message in global buffer
    for (size_t i = 0; i < server->globalBufferSize; i++) {
        BufferedMessage *msg = &server->globalMessageBuffer[i];
        if (msg->seqNum == seqNum) {
            // Remove this client from pending list
            for (size_t j = 0; j < msg->pendingClientCount; j++) {
                if (msg->pendingClientFds[j] == client->fd) {
                    // Remove by shifting
                    for (size_t k = j; k < msg->pendingClientCount - 1; k++) {
                        msg->pendingClientFds[k] = msg->pendingClientFds[k + 1];
                    }
                    msg->pendingClientCount--;
                    DEBUG_LOG("Message %" PRIu64
                              " now has %zu pending clients\n",
                              seqNum, msg->pendingClientCount);
                    break;
                }
            }

            // Update client's last seq num
            for (size_t j = 0; j < client->subscriptionCount; j++) {
                if (client->subscriptions[j].active &&
                    client->subscriptions[j].lastSeqNum < seqNum) {
                    client->subscriptions[j].lastSeqNum = seqNum;
                }
            }
            return;
        }
    }

    DEBUG_LOG("Message seqNum=%" PRIu64 " not found in buffer\n", seqNum);
}

void cleanupOldMessages(TrieServer *server) {
    time_t now = time(NULL);
    size_t removed = 0;

    // Remove messages that have no pending clients or are too old (>5 minutes)
    for (size_t i = 0; i < server->globalBufferSize;) {
        BufferedMessage *msg = &server->globalMessageBuffer[i];

        bool shouldRemove =
            (msg->pendingClientCount == 0) || (now - msg->timestamp > 300);

        if (shouldRemove) {
            // Phase 3: Use buffer pool free
            bufferPoolFree(&server->bufferPool, msg->payload, msg->payloadLen);
            free(msg->pendingClientFds);

            // Shift remaining messages
            for (size_t j = i; j < server->globalBufferSize - 1; j++) {
                server->globalMessageBuffer[j] =
                    server->globalMessageBuffer[j + 1];
            }
            server->globalBufferSize--;
            removed++;
        } else {
            i++;
        }
    }

    if (removed > 0) {
        DEBUG_LOG("Cleaned up %zu old messages from buffer\n", removed);
    }
}

// ============================================================================
// SERVER INITIALIZATION
// ============================================================================

bool serverInit(TrieServer *server, uint16_t port, const char *authToken,
                const char *saveFilePath) {
    memset(server, 0, sizeof(TrieServer));

    server->port = port;
    server->running = false;
    server->startTime = time(NULL);
    server->lastHeartbeat = time(NULL);

    // Initialize pub/sub state
    server->totalPublishes = 0;
    server->totalNotificationsSent = 0;
    server->totalLiveSubscriptions = 0;
    server->nextClientId = 1000; // Start from 1000 to avoid conflicts
    server->globalMessageBuffer = NULL;
    server->globalBufferSize = 0;
    server->globalBufferCapacity = 0;
    server->nextGlobalSeqNum = 1;

    // Initialize client manager (dynamic, no hard limit)
    if (!clientMgrInit(&server->clientMgr, 128)) {
        fprintf(stderr, "Failed to initialize client manager\n");
        trieFree(&server->trie);
        if (server->authToken) {
            free(server->authToken);
        }
        if (server->saveFilePath) {
            free(server->saveFilePath);
        }
        return false;
    }

    // Initialize message pools (Phase 3)
    if (!msgPoolInit(&server->msgPool, 64)) {
        fprintf(stderr, "Failed to initialize message pool\n");
        clientMgrDestroy(&server->clientMgr);
        trieFree(&server->trie);
        if (server->authToken) {
            free(server->authToken);
        }
        if (server->saveFilePath) {
            free(server->saveFilePath);
        }
        return false;
    }

    if (!bufferPoolInit(&server->bufferPool)) {
        fprintf(stderr, "Failed to initialize buffer pool\n");
        msgPoolDestroy(&server->msgPool);
        clientMgrDestroy(&server->clientMgr);
        trieFree(&server->trie);
        if (server->authToken) {
            free(server->authToken);
        }
        if (server->saveFilePath) {
            free(server->saveFilePath);
        }
        return false;
    }

    // Authentication
    if (authToken && strlen(authToken) > 0) {
        server->authToken = strdup(authToken);
        server->requireAuth = true;
    } else {
        server->authToken = NULL;
        server->requireAuth = false;
    }

    // Save file
    if (saveFilePath) {
        server->saveFilePath = strdup(saveFilePath);
    }

    // Initialize trie
    trieInit(&server->trie);

    // Load existing data if save file exists
    if (server->saveFilePath && access(server->saveFilePath, F_OK) == 0) {
        printf("Loading existing trie from %s...\n", server->saveFilePath);
        if (!trieLoad(&server->trie, server->saveFilePath)) {
            fprintf(stderr, "Warning: Failed to load trie from %s\n",
                    server->saveFilePath);
        }
    }

    // Create listen socket
    server->listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listenFd < 0) {
        perror("socket");
        trieFree(&server->trie);
        if (server->authToken) {
            free(server->authToken);
        }
        if (server->saveFilePath) {
            free(server->saveFilePath);
        }
        return false;
    }

    // Set socket options
    int opt = 1;
    setsockopt(server->listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server->listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server->listenFd);
        trieFree(&server->trie);
        if (server->authToken) {
            free(server->authToken);
        }
        if (server->saveFilePath) {
            free(server->saveFilePath);
        }
        return false;
    }

    // Listen
    if (listen(server->listenFd, 128) < 0) {
        perror("listen");
        close(server->listenFd);
        trieFree(&server->trie);
        if (server->authToken) {
            free(server->authToken);
        }
        if (server->saveFilePath) {
            free(server->saveFilePath);
        }
        return false;
    }

    setNonBlocking(server->listenFd);

    // Create event queue (epoll on Linux, kqueue on BSD/macOS)
    server->eventFd = event_queue_create();
    if (server->eventFd < 0) {
        perror("event_queue_create");
        close(server->listenFd);
        trieFree(&server->trie);
        if (server->authToken) {
            free(server->authToken);
        }
        if (server->saveFilePath) {
            free(server->saveFilePath);
        }
        return false;
    }

    // Register listen socket with event queue
    printf(
        "DEBUG: Registering listen socket (fd=%d) with event queue (fd=%d)\n",
        server->listenFd, server->eventFd);
    if (event_queue_add(server->eventFd, server->listenFd, EVENT_READ,
                        (void *)(intptr_t)server->listenFd) < 0) {
        perror("event_queue_add: listen socket");
        close(server->eventFd);
        close(server->listenFd);
        trieFree(&server->trie);
        if (server->authToken) {
            free(server->authToken);
        }
        if (server->saveFilePath) {
            free(server->saveFilePath);
        }
        return false;
    }
    printf("DEBUG: Listen socket registered successfully\n");

    printf("Trie server listening on port %d (using "
#ifdef USE_EPOLL
           "epoll"
#else
           "kqueue"
#endif
           " for high-performance async I/O)\n",
           port);
    if (server->requireAuth) {
        printf("Authentication: ENABLED\n");
    }
    if (server->saveFilePath) {
        printf("Auto-save: %s (every %d seconds or %d commands)\n",
               server->saveFilePath, AUTO_SAVE_INTERVAL, AUTO_SAVE_THRESHOLD);
    }

    return true;
}

// ============================================================================
// MAIN EVENT LOOP
// ============================================================================

static volatile bool g_shutdown = false;

static void signalHandler(int sig) {
    (void)sig;
    g_shutdown = true;
}

void serverRun(TrieServer *server) {
    server->running = true;
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    fprintf(stderr, "Server ready. Press Ctrl+C to stop.\n");
    DEBUG_LOG("Entering event loop with eventFd=%d, listenFd=%d\n",
              server->eventFd, server->listenFd);

#define MAX_EVENTS 64
    event_t events[MAX_EVENTS];

    int loopCount = 0;
    while (server->running && !g_shutdown) {
        // Wait for events (1 second timeout)
        int nfds = event_queue_wait(server->eventFd, events, MAX_EVENTS, 1000);

        if (loopCount < 5 || nfds > 0) {
            DEBUG_LOG("event_queue_wait iteration %d returned nfds=%d\n",
                      loopCount, nfds);
        }
        loopCount++;

        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("event_queue_wait");
            break;
        }

        if (nfds > 0) {
            DEBUG_LOG("event_queue_wait returned %d events\n", nfds);
        }

        // Process all events
        for (int n = 0; n < nfds; n++) {
            int fd = (int)(intptr_t)event_get_data(&events[n]);
            uint32_t evflags = event_get_events(&events[n]);
            DEBUG_LOG("Event on fd=%d (events=0x%x)\n", fd, evflags);

            // New connection on listen socket
            if (fd == server->listenFd) {
                DEBUG_LOG("New connection attempt on listen socket\n");
                struct sockaddr_in clientAddr;
                socklen_t addrLen = sizeof(clientAddr);
                int clientFd = accept(server->listenFd,
                                      (struct sockaddr *)&clientAddr, &addrLen);

                if (clientFd >= 0) {
                    setNonBlocking(clientFd);

                    // Allocate client from pool (auto-expands if needed)
                    uint64_t clientId = server->nextClientId++;
                    ClientConnection *client = clientMgrAllocate(
                        &server->clientMgr, clientFd, clientId);

                    if (client) {
                        // clientMgrAllocate already initialized fd, clientId,
                        // state, and pub/sub Just set additional fields
                        client->authenticated = !server->requireAuth;
                        client->lastActivity = time(NULL);
                        client->rateLimitWindowStart = time(NULL);

                        // Add to client manager (dual hash tables + active
                        // list)
                        if (clientMgrAdd(&server->clientMgr, client)) {
                            // Register client with event queue for
                            // edge-triggered reading
                            if (event_queue_add(server->eventFd, clientFd,
                                                EVENT_READ | EVENT_ET,
                                                (void *)(intptr_t)clientFd) <
                                0) {
                                perror("event_queue_add: client socket");
                                close(clientFd);
                                clientMgrRemove(&server->clientMgr, client);
                                clientMgrFree(&server->clientMgr, client);
                            } else {
                                server->totalConnections++;
                                printf("New connection from %s (client ID: "
                                       "%" PRIu64 ", total "
                                       "connections: %" PRIu64 ")\n",
                                       inet_ntoa(clientAddr.sin_addr), clientId,
                                       server->totalConnections);
                            }
                        } else {
                            fprintf(stderr,
                                    "Failed to add client to manager\n");
                            close(clientFd);
                            clientMgrFree(&server->clientMgr, client);
                        }
                    } else {
                        fprintf(stderr,
                                "Failed to allocate client (out of memory)\n");
                        close(clientFd);
                    }
                }
                continue;
            }

            // Find client for this fd (O(1) hash table lookup)
            ClientConnection *client = clientMgrGetByFd(&server->clientMgr, fd);

            if (!client) {
                continue;
            }

            bool active = false;

            // Handle read events
            if (evflags & EPOLLIN) {
                handleClient(server, client);
                active = true;
            }

            // Handle write events
            if (evflags & EPOLLOUT) {
                DEBUG_LOG("EPOLLOUT event on fd=%d, state=%d\n", fd,
                          client->state);
            }
            if (evflags & EPOLLOUT && client->state == CONN_WRITING_RESPONSE) {
                DEBUG_LOG(
                    "Writing response, writeOffset=%zu, writeLength=%zu\n",
                    client->writeOffset, client->writeLength);
                ssize_t sent =
                    write(client->fd, client->writeBuffer + client->writeOffset,
                          client->writeLength - client->writeOffset);
                DEBUG_LOG("write() returned %zd (errno=%d)\n", sent,
                          sent < 0 ? errno : 0);
                if (sent > 0) {
                    client->writeOffset += sent;
                    DEBUG_LOG("Sent %zd bytes, writeOffset now %zu\n", sent,
                              client->writeOffset);
                    if (client->writeOffset >= client->writeLength) {
                        // Response fully sent, switch back to reading
                        client->state = CONN_READING_LENGTH;
                        client->readOffset = 0;
                        client->writeOffset = 0;
                        client->writeLength = 0;

                        // Modify event queue to monitor for read only
                        // (edge-triggered)
                        event_queue_mod(server->eventFd, client->fd,
                                        EVENT_READ | EVENT_ET,
                                        (void *)(intptr_t)client->fd);
                    }
                    active = true;
                } else if (sent < 0 && errno != EAGAIN &&
                           errno != EWOULDBLOCK) {
                    disconnectClient(server, client);
                }
            }

            if (active) {
                client->lastActivity = time(NULL);
            }
        }

        // Check for client timeouts (periodic maintenance)
        time_t now = time(NULL);
        for (size_t i = 0; i < server->clientMgr.activeCount; i++) {
            ClientConnection *client = server->clientMgr.activeList[i];
            if (now - client->lastActivity > CLIENT_TIMEOUT) {
                printf("Client %" PRIu64 " timed out\n", client->clientId);
                disconnectClient(server, client);
            }
        }

        // Auto-save check
        if (server->saveFilePath) {
            bool shouldSave = false;

            if (now - server->lastSaveTime >= AUTO_SAVE_INTERVAL) {
                shouldSave = true;
            }
            if (server->commandsSinceLastSave >= AUTO_SAVE_THRESHOLD) {
                shouldSave = true;
            }

            if (shouldSave && server->commandsSinceLastSave > 0) {
                printf("Auto-saving trie (%" PRIu64
                       " commands since last save)...\n",
                       server->commandsSinceLastSave);
                if (trieSave(&server->trie, server->saveFilePath)) {
                    server->lastSaveTime = now;
                    server->commandsSinceLastSave = 0;
                } else {
                    fprintf(stderr, "Auto-save failed!\n");
                }
            }
        }

        // Cleanup old messages (every 60 seconds)
        static time_t lastCleanup = 0;
        if (lastCleanup == 0) {
            lastCleanup = now;
        }
        if (now - lastCleanup >= 60) {
            cleanupOldMessages(server);
            lastCleanup = now;
        }

        // Send heartbeats (every 30 seconds)
        if (now - server->lastHeartbeat >= 30) {
            for (size_t i = 0; i < server->clientMgr.activeCount; i++) {
                ClientConnection *client = server->clientMgr.activeList[i];
                if (client->authenticated && client->subscriptionCount > 0) {
                    // Send heartbeat to subscribed clients
                    uint8_t heartbeat = MSG_HEARTBEAT;
                    sendResponse(server->eventFd, client, STATUS_OK, &heartbeat,
                                 1);
                }
            }
            server->lastHeartbeat = now;
        }
    }

    printf("\nShutting down gracefully...\n");
}

void serverShutdown(TrieServer *server) {
    // Close all client connections
    for (size_t i = 0; i < server->clientMgr.activeCount; i++) {
        ClientConnection *client = server->clientMgr.activeList[i];
        resetClient(server->eventFd, client);
    }

    // Destroy client manager
    clientMgrDestroy(&server->clientMgr);

    // Destroy message pools (Phase 3)
    msgPoolDestroy(&server->msgPool);
    bufferPoolDestroy(&server->bufferPool);

    // Final save
    if (server->saveFilePath && server->commandsSinceLastSave > 0) {
        printf("Saving trie before shutdown...\n");
        trieSave(&server->trie, server->saveFilePath);
    }

    // Close listen socket
    if (server->listenFd >= 0) {
        close(server->listenFd);
    }

    // Close event queue fd
    if (server->eventFd >= 0) {
        close(server->eventFd);
    }

    // Free resources
    trieFree(&server->trie);
    free(server->authToken);
    free(server->saveFilePath);

    printf("Server shutdown complete.\n");
    printf("Statistics:\n");
    printf("  Total connections: %" PRIu64 "\n", server->totalConnections);
    printf("  Total commands: %" PRIu64 "\n", server->totalCommands);
    printf("  Total errors: %" PRIu64 "\n", server->totalErrors);
    printf("  Uptime: %" PRId64 " seconds\n",
           (int64_t)(time(NULL) - server->startTime));
}

// ============================================================================
// PROTOCOL HANDLING
// ============================================================================

void sendResponse(int eventFd, ClientConnection *client, StatusCode status,
                  const uint8_t *data, size_t dataLen) {
    DEBUG_LOG("sendResponse called - fd=%d status=0x%02X dataLen=%zu\n",
              client->fd, status, dataLen);
    // Build response: [Length:varint][Status:1byte][Data]
    uint8_t tempBuf[MAX_MESSAGE_SIZE];
    size_t offset = 0;

    // Reserve space for length (will fill in later)
    size_t lengthOffset = 0;
    offset += 5; // Max varint size for length

    // Status code
    tempBuf[offset++] = (uint8_t)status;

    // Data payload
    if (data && dataLen > 0) {
        if (offset + dataLen > sizeof(tempBuf)) {
            return; // Too large
        }
        memcpy(tempBuf + offset, data, dataLen);
        offset += dataLen;
    }

    // Calculate message length (status + data)
    uint64_t messageLen = (offset - 5);

    // Write length at beginning
    size_t lengthBytes = varintTaggedPut64(tempBuf + lengthOffset, messageLen);

    // Copy to write buffer (length + status + data)
    size_t totalSize = lengthBytes + messageLen;
    if (totalSize > WRITE_BUFFER_SIZE) {
        return; // Response too large
    }

    // Shift message to remove extra length padding
    memmove(tempBuf + lengthBytes, tempBuf + 5, messageLen);
    memcpy(client->writeBuffer, tempBuf, totalSize);
    client->writeLength = totalSize;
    client->writeOffset = 0;
    client->state = CONN_WRITING_RESPONSE;

    // Modify event queue to monitor for both read and write (edge-triggered)
    DEBUG_LOG(
        "Modifying event queue for fd=%d to EVENT_READ|EVENT_WRITE|EVENT_ET, "
        "writeLength=%zu\n",
        client->fd, client->writeLength);
    (void)event_queue_mod(eventFd, client->fd,
                          EVENT_READ | EVENT_WRITE | EVENT_ET,
                          (void *)(intptr_t)client->fd);
}

bool processCommand(TrieServer *server, ClientConnection *client,
                    const uint8_t *data, size_t length) {
    DEBUG_LOG("processCommand called - length=%zu\n", length);
    if (length == 0) {
        sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
        return false;
    }

    CommandType cmd = (CommandType)data[0];
    DEBUG_LOG("Command ID: 0x%02X\n", cmd);
    size_t offset = 1;

    // Check authentication
    if (server->requireAuth && !client->authenticated && cmd != CMD_AUTH) {
        sendResponse(server->eventFd, client, STATUS_AUTH_REQUIRED, NULL, 0);
        return false;
    }

    // Check rate limit
    if (!checkRateLimit(client)) {
        sendResponse(server->eventFd, client, STATUS_RATE_LIMITED, NULL, 0);
        server->totalErrors++;
        return false;
    }

    server->totalCommands++;
    server->commandsSinceLastSave++;

    uint8_t responseBuf[MAX_MESSAGE_SIZE];

    switch (cmd) {
    case CMD_PING: {
        // PING - just respond OK
        sendResponse(server->eventFd, client, STATUS_OK, NULL, 0);
        break;
    }

    case CMD_ADD: {
        // ADD
        // <pattern_len:varint><pattern:bytes><subscriber_id:varint><subscriber_name_len:varint><subscriber_name:bytes>
        uint64_t patternLen;
        varintWidth width = varintTaggedGet64(data + offset, &patternLen);
        if (width == VARINT_WIDTH_INVALID) {
            fprintf(stderr,
                    "Error: Invalid varint for patternLen in CMD_ADD\n");
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        if (offset + patternLen > length) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }

        char pattern[MAX_PATTERN_LENGTH];
        secureBinaryCopy(pattern, MAX_PATTERN_LENGTH, data + offset,
                         patternLen);
        offset += patternLen;

        uint64_t subscriberId;
        width = varintTaggedGet64(data + offset, &subscriberId);
        if (width == VARINT_WIDTH_INVALID) {
            fprintf(stderr,
                    "Error: Invalid varint for subscriberId in CMD_ADD\n");
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        uint64_t subscriberNameLen;
        width = varintTaggedGet64(data + offset, &subscriberNameLen);
        if (width == VARINT_WIDTH_INVALID) {
            fprintf(stderr,
                    "Error: Invalid varint for subscriberNameLen in CMD_ADD\n");
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        if (offset + subscriberNameLen > length) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }

        char subscriberName[MAX_SUBSCRIBER_NAME];
        secureBinaryCopy(subscriberName, MAX_SUBSCRIBER_NAME, data + offset,
                         subscriberNameLen);

        if (trieInsert(&server->trie, pattern, (uint32_t)subscriberId,
                       subscriberName)) {
            sendResponse(server->eventFd, client, STATUS_OK, NULL, 0);
        } else {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
        }
        break;
    }

    case CMD_REMOVE: {
        // REMOVE <pattern_len:varint><pattern:bytes>
        uint64_t patternLen;
        varintWidth width = varintTaggedGet64(data + offset, &patternLen);
        if (width == VARINT_WIDTH_INVALID) {
            fprintf(stderr,
                    "Error: Invalid varint for patternLen in CMD_REMOVE\n");
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        if (offset + patternLen > length) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }

        char pattern[MAX_PATTERN_LENGTH];
        secureBinaryCopy(pattern, MAX_PATTERN_LENGTH, data + offset,
                         patternLen);

        if (trieRemovePattern(&server->trie, pattern)) {
            sendResponse(server->eventFd, client, STATUS_OK, NULL, 0);
        } else {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
        }
        break;
    }

    case CMD_SUBSCRIBE: {
        // SUBSCRIBE
        // <pattern_len:varint><pattern:bytes><subscriber_id:varint><subscriber_name_len:varint><subscriber_name:bytes>
        uint64_t patternLen;
        varintWidth width = varintTaggedGet64(data + offset, &patternLen);
        if (width == VARINT_WIDTH_INVALID) {
            fprintf(stderr,
                    "Error: Invalid varint for patternLen in CMD_SUBSCRIBE\n");
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        if (offset + patternLen > length) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }

        char pattern[MAX_PATTERN_LENGTH];
        secureBinaryCopy(pattern, MAX_PATTERN_LENGTH, data + offset,
                         patternLen);
        offset += patternLen;

        uint64_t subscriberId;
        width = varintTaggedGet64(data + offset, &subscriberId);
        if (width == VARINT_WIDTH_INVALID) {
            fprintf(
                stderr,
                "Error: Invalid varint for subscriberId in CMD_SUBSCRIBE\n");
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        uint64_t subscriberNameLen;
        width = varintTaggedGet64(data + offset, &subscriberNameLen);
        if (width == VARINT_WIDTH_INVALID) {
            fprintf(stderr, "Error: Invalid varint for subscriberNameLen in "
                            "CMD_SUBSCRIBE\n");
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        if (offset + subscriberNameLen > length) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }

        char subscriberName[MAX_SUBSCRIBER_NAME];
        secureBinaryCopy(subscriberName, MAX_SUBSCRIBER_NAME, data + offset,
                         subscriberNameLen);

        // CMD_SUBSCRIBE is the same as CMD_ADD - both insert pattern with
        // subscriber
        if (trieInsert(&server->trie, pattern, (uint32_t)subscriberId,
                       subscriberName)) {
            sendResponse(server->eventFd, client, STATUS_OK, NULL, 0);
        } else {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
        }
        break;
    }

    case CMD_UNSUBSCRIBE: {
        // UNSUBSCRIBE <pattern_len:varint><pattern:bytes><subscriber_id:varint>
        uint64_t patternLen;
        varintWidth width = varintTaggedGet64(data + offset, &patternLen);
        if (width == VARINT_WIDTH_INVALID) {
            fprintf(
                stderr,
                "Error: Invalid varint for patternLen in CMD_UNSUBSCRIBE\n");
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        if (offset + patternLen > length) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }

        char pattern[MAX_PATTERN_LENGTH];
        secureBinaryCopy(pattern, MAX_PATTERN_LENGTH, data + offset,
                         patternLen);
        offset += patternLen;

        uint64_t subscriberId;
        width = varintTaggedGet64(data + offset, &subscriberId);
        if (width == VARINT_WIDTH_INVALID) {
            fprintf(
                stderr,
                "Error: Invalid varint for subscriberId in CMD_UNSUBSCRIBE\n");
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;
        (void)offset; // Suppress unused value warning

        if (trieRemoveSubscriber(&server->trie, pattern,
                                 (uint32_t)subscriberId)) {
            sendResponse(server->eventFd, client, STATUS_OK, NULL, 0);
        } else {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
        }
        break;
    }

    case CMD_MATCH: {
        // MATCH <input_len:varint><input:bytes>
        // Response:
        // <count:varint>[<subscriber_id:varint><subscriber_name_len:varint><subscriber_name:bytes>]*
        uint64_t inputLen;
        varintWidth width = varintTaggedGet64(data + offset, &inputLen);
        if (width == VARINT_WIDTH_INVALID) {
            fprintf(stderr,
                    "Error: Invalid varint for inputLen in CMD_MATCH\n");
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        if (offset + inputLen > length) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }

        char input[MAX_PATTERN_LENGTH];
        secureBinaryCopy(input, MAX_PATTERN_LENGTH, data + offset, inputLen);

        MatchResult result;
        trieMatch(&server->trie, input, &result);

        // Build response
        size_t pos = 0;
        pos += varintTaggedPut64(responseBuf + pos, result.count);

        for (size_t i = 0; i < result.count; i++) {
            pos +=
                varintTaggedPut64(responseBuf + pos, result.subscriberIds[i]);
            size_t nameLen = strlen(result.subscriberNames[i]);
            pos += varintTaggedPut64(responseBuf + pos, nameLen);
            memcpy(responseBuf + pos, result.subscriberNames[i], nameLen);
            pos += nameLen;
        }

        sendResponse(server->eventFd, client, STATUS_OK, responseBuf, pos);
        break;
    }

    case CMD_LIST: {
        // LIST - return all patterns
        // Response: <count:varint>[<pattern_len:varint><pattern:bytes>]*
        char patterns[MAX_SUBSCRIBERS][MAX_PATTERN_LENGTH];
        size_t count;
        trieListPatterns(&server->trie, patterns, &count, MAX_SUBSCRIBERS);

        size_t pos = 0;
        pos += varintTaggedPut64(responseBuf + pos, count);

        for (size_t i = 0; i < count; i++) {
            size_t patternLen = strlen(patterns[i]);
            pos += varintTaggedPut64(responseBuf + pos, patternLen);
            memcpy(responseBuf + pos, patterns[i], patternLen);
            pos += patternLen;
        }

        sendResponse(server->eventFd, client, STATUS_OK, responseBuf, pos);
        break;
    }

    case CMD_AUTH: {
        // AUTH <token_len:varint><token:bytes>
        if (!server->requireAuth) {
            sendResponse(server->eventFd, client, STATUS_OK, NULL, 0);
            break;
        }

        uint64_t tokenLen;
        varintWidth width = varintTaggedGet64(data + offset, &tokenLen);
        if (width == VARINT_WIDTH_INVALID) {
            fprintf(stderr, "Error: Invalid varint for tokenLen in CMD_AUTH\n");
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        if (offset + tokenLen > length) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }

        if (tokenLen == strlen(server->authToken) &&
            memcmp(data + offset, server->authToken, tokenLen) == 0) {
            client->authenticated = true;
            sendResponse(server->eventFd, client, STATUS_OK, NULL, 0);
        } else {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
        }
        break;
    }

    case CMD_STATS: {
        // STATS - return server statistics
        // Format: patterns:varint, subscribers:varint, nodes:varint,
        //         connections:varint, commands:varint, uptime:varint
        size_t pos = 0;
        size_t totalNodes, terminalNodes, wildcardNodes, maxDepth;
        trieStats(&server->trie, &totalNodes, &terminalNodes, &wildcardNodes,
                  &maxDepth);

        pos += varintTaggedPut64(responseBuf + pos, server->trie.patternCount);
        pos +=
            varintTaggedPut64(responseBuf + pos, server->trie.subscriberCount);
        pos += varintTaggedPut64(responseBuf + pos, totalNodes);
        pos += varintTaggedPut64(responseBuf + pos, server->totalConnections);
        pos += varintTaggedPut64(responseBuf + pos, server->totalCommands);
        pos += varintTaggedPut64(responseBuf + pos,
                                 time(NULL) - server->startTime);

        sendResponse(server->eventFd, client, STATUS_OK, responseBuf, pos);
        break;
    }

    case CMD_SAVE: {
        // SAVE - trigger manual save
        if (server->saveFilePath) {
            if (trieSave(&server->trie, server->saveFilePath)) {
                server->lastSaveTime = time(NULL);
                server->commandsSinceLastSave = 0;
                sendResponse(server->eventFd, client, STATUS_OK, NULL, 0);
            } else {
                sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
            }
        } else {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
        }
        break;
    }

    case CMD_SHUTDOWN: {
        // SHUTDOWN - gracefully shutdown the server
        // Send OK response first, then initiate shutdown
        sendResponse(server->eventFd, client, STATUS_OK, NULL, 0);

        // Force synchronous flush of response by writing immediately
        while (client->writeOffset < client->writeLength) {
            ssize_t n =
                write(client->fd, client->writeBuffer + client->writeOffset,
                      client->writeLength - client->writeOffset);
            if (n > 0) {
                client->writeOffset += n;
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                break; // Write error, but continue shutdown anyway
            }
        }

        // Save trie if auto-save is enabled
        if (server->saveFilePath) {
            printf("Saving trie before shutdown...\n");
            trieSave(&server->trie, server->saveFilePath);
        }

        // Trigger graceful shutdown
        printf("Server shutdown requested by client\n");
        server->running = false;
        break;
    }

    case CMD_PUBLISH: {
        // PUBLISH
        // <pattern_len:varint><pattern:bytes><payload_len:varint><payload:bytes>
        uint64_t patternLen;
        varintWidth width = varintTaggedGet64(data + offset, &patternLen);
        if (width == VARINT_WIDTH_INVALID) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        if (offset + patternLen > length) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }

        char pattern[MAX_PATTERN_LENGTH];
        secureBinaryCopy(pattern, MAX_PATTERN_LENGTH, data + offset,
                         patternLen);
        offset += patternLen;

        uint64_t payloadLen;
        width = varintTaggedGet64(data + offset, &payloadLen);
        if (width == VARINT_WIDTH_INVALID) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        if (offset + payloadLen > length) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }

        const uint8_t *payload = data + offset;

        // Publish the message to all subscribed clients
        const char *publisherName =
            client->hasIdentity ? client->clientName : "anonymous";
        if (publishMessage(server, pattern, payload, payloadLen,
                           client->clientId, publisherName)) {
            sendResponse(server->eventFd, client, STATUS_OK, NULL, 0);
        } else {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
        }
        break;
    }

    case CMD_SUBSCRIBE_LIVE: {
        // SUBSCRIBE_LIVE
        // <pattern_len:varint><pattern:bytes><qos:1byte><client_id:varint><client_name_len:varint><client_name:bytes>
        uint64_t patternLen;
        varintWidth width = varintTaggedGet64(data + offset, &patternLen);
        if (width == VARINT_WIDTH_INVALID) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        if (offset + patternLen > length) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }

        char pattern[MAX_PATTERN_LENGTH];
        secureBinaryCopy(pattern, MAX_PATTERN_LENGTH, data + offset,
                         patternLen);
        offset += patternLen;

        // QoS level
        if (offset >= length) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        QoSLevel qos = (QoSLevel)data[offset++];

        // Optional: client ID (if client wants to set their own)
        uint64_t clientId;
        width = varintTaggedGet64(data + offset, &clientId);
        if (width == VARINT_WIDTH_INVALID) {
            clientId = client->clientId; // Use existing
        } else {
            offset += width;
            if (clientId != client->clientId && clientId != 0) {
                client->clientId = clientId;
            }
        }

        // Optional: client name
        uint64_t clientNameLen;
        width = varintTaggedGet64(data + offset, &clientNameLen);
        if (width != VARINT_WIDTH_INVALID && offset + width <= length) {
            offset += width;
            if (offset + clientNameLen <= length && clientNameLen > 0) {
                secureBinaryCopy(client->clientName, MAX_SUBSCRIBER_NAME,
                                 data + offset, clientNameLen);
                client->hasIdentity = true;
            }
        }

        // Add to trie as a subscriber
        const char *clientName =
            client->hasIdentity ? client->clientName : "anonymous";
        if (trieInsert(&server->trie, pattern, client->clientId, clientName)) {
            // Add to client's subscription list
            if (addClientSubscription(client, pattern, qos)) {
                server->totalLiveSubscriptions++;

                // Send confirmation
                size_t pos = 0;
                pos += varintTaggedPut64(responseBuf + pos, client->clientId);
                sendResponse(server->eventFd, client, STATUS_OK, responseBuf,
                             pos);
            } else {
                sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
            }
        } else {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
        }
        break;
    }

    case CMD_GET_SUBSCRIPTIONS: {
        // GET_SUBSCRIPTIONS - return client's current subscriptions
        // Response:
        // <count:varint>[<pattern_len:varint><pattern:bytes><qos:1byte>]*
        size_t pos = 0;

        // Count active subscriptions
        size_t activeCount = 0;
        for (size_t i = 0; i < client->subscriptionCount; i++) {
            if (client->subscriptions[i].active) {
                activeCount++;
            }
        }

        pos += varintTaggedPut64(responseBuf + pos, activeCount);

        for (size_t i = 0; i < client->subscriptionCount; i++) {
            if (!client->subscriptions[i].active) {
                continue;
            }

            size_t patternLen = strlen(client->subscriptions[i].pattern);
            pos += varintTaggedPut64(responseBuf + pos, patternLen);
            memcpy(responseBuf + pos, client->subscriptions[i].pattern,
                   patternLen);
            pos += patternLen;
            responseBuf[pos++] = (uint8_t)client->subscriptions[i].qos;
        }

        sendResponse(server->eventFd, client, STATUS_OK, responseBuf, pos);
        break;
    }

    case CMD_SET_QOS: {
        // SET_QOS <qos:1byte>
        if (offset >= length) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }

        QoSLevel qos = (QoSLevel)data[offset];
        client->defaultQos = qos;

        sendResponse(server->eventFd, client, STATUS_OK, NULL, 0);
        break;
    }

    case CMD_ACK: {
        // ACK <seqNum:varint>
        uint64_t seqNum;
        varintWidth width = varintTaggedGet64(data + offset, &seqNum);
        if (width == VARINT_WIDTH_INVALID) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }

        acknowledgeMessage(server, client, seqNum);
        sendResponse(server->eventFd, client, STATUS_OK, NULL, 0);
        break;
    }

    case CMD_GET_BACKLOG: {
        // GET_BACKLOG - retrieve missed messages (for reconnection)
        // Response:
        // <count:varint>[<seqNum:varint><pattern:string><payload:bytes>]*
        size_t pos = 0;

        // Find messages this client needs to acknowledge
        size_t backlogCount = 0;
        for (size_t i = 0; i < server->globalBufferSize; i++) {
            const BufferedMessage *msg = &server->globalMessageBuffer[i];
            for (size_t j = 0; j < msg->pendingClientCount; j++) {
                if (msg->pendingClientFds[j] == client->fd) {
                    backlogCount++;
                    break;
                }
            }
        }

        pos += varintTaggedPut64(responseBuf + pos, backlogCount);

        // Send up to 100 messages
        size_t sent = 0;
        for (size_t i = 0; i < server->globalBufferSize && sent < 100; i++) {
            const BufferedMessage *msg = &server->globalMessageBuffer[i];

            bool isPending = false;
            for (size_t j = 0; j < msg->pendingClientCount; j++) {
                if (msg->pendingClientFds[j] == client->fd) {
                    isPending = true;
                    break;
                }
            }

            if (!isPending) {
                continue;
            }

            // Add message to response
            pos += varintTaggedPut64(responseBuf + pos, msg->seqNum);

            size_t patternLen = strlen(msg->pattern);
            pos += varintTaggedPut64(responseBuf + pos, patternLen);
            memcpy(responseBuf + pos, msg->pattern, patternLen);
            pos += patternLen;

            pos += varintTaggedPut64(responseBuf + pos, msg->payloadLen);
            if (pos + msg->payloadLen > sizeof(responseBuf)) {
                break; // Response too large
            }
            memcpy(responseBuf + pos, msg->payload, msg->payloadLen);
            pos += msg->payloadLen;

            sent++;
        }

        sendResponse(server->eventFd, client, STATUS_OK, responseBuf, pos);
        break;
    }

    case CMD_SUBSCRIBE_BATCH: {
        // SUBSCRIBE_BATCH
        // <count:varint>[<pattern_len:varint><pattern:bytes>]*<qos:1byte>
        uint64_t count;
        varintWidth width = varintTaggedGet64(data + offset, &count);
        if (width == VARINT_WIDTH_INVALID) {
            sendResponse(server->eventFd, client, STATUS_ERROR, NULL, 0);
            server->totalErrors++;
            return false;
        }
        offset += width;

        // Read all patterns
        char patterns[MAX_SUBSCRIPTIONS_PER_CLIENT][MAX_PATTERN_LENGTH];
        size_t patternCount = 0;

        for (uint64_t i = 0;
             i < count && patternCount < MAX_SUBSCRIPTIONS_PER_CLIENT; i++) {
            uint64_t patternLen;
            width = varintTaggedGet64(data + offset, &patternLen);
            if (width == VARINT_WIDTH_INVALID) {
                break;
            }
            offset += width;

            if (offset + patternLen > length) {
                break;
            }

            secureBinaryCopy(patterns[patternCount], MAX_PATTERN_LENGTH,
                             data + offset, patternLen);
            offset += patternLen;
            patternCount++;
        }

        // QoS level
        QoSLevel qos = QOS_AT_MOST_ONCE;
        if (offset < length) {
            qos = (QoSLevel)data[offset];
        }

        // Subscribe to all patterns
        size_t successCount = 0;
        const char *clientName =
            client->hasIdentity ? client->clientName : "anonymous";

        for (size_t i = 0; i < patternCount; i++) {
            if (trieInsert(&server->trie, patterns[i], client->clientId,
                           clientName)) {
                if (addClientSubscription(client, patterns[i], qos)) {
                    successCount++;
                    server->totalLiveSubscriptions++;
                }
            }
        }

        // Send response with success count
        size_t pos = 0;
        pos += varintTaggedPut64(responseBuf + pos, successCount);
        sendResponse(server->eventFd, client, STATUS_OK, responseBuf, pos);
        break;
    }

    default:
        sendResponse(server->eventFd, client, STATUS_INVALID_CMD, NULL, 0);
        server->totalErrors++;
        return false;
    }

    return true;
}

void handleClient(TrieServer *server, ClientConnection *client) {
    // In edge-triggered mode, we must read until EAGAIN
    while (client->state == CONN_READING_LENGTH ||
           client->state == CONN_READING_MESSAGE) {
        ssize_t bytesRead =
            read(client->fd, client->readBuffer + client->readOffset,
                 READ_BUFFER_SIZE - client->readOffset);

        DEBUG_LOG("handleClient fd=%d bytesRead=%zd errno=%d state=%d\n",
                  client->fd, bytesRead, errno, client->state);

        if (bytesRead <= 0) {
            if (bytesRead < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                DEBUG_LOG("EAGAIN, returning\n");
                return; // No more data available
            }
            // Connection closed or error
            DEBUG_LOG("Connection closed or error, disconnecting client\n");
            disconnectClient(server, client);
            return;
        }

        DEBUG_LOG("Read %zd bytes, readOffset=%zu\n", bytesRead,
                  client->readOffset);
#ifdef ENABLE_DEBUG_LOGGING
        // Print hex dump of what we read
        fprintf(stderr, "DEBUG: Data (hex): ");
        for (ssize_t i = 0; i < bytesRead && i < 16; i++) {
            fprintf(stderr, "%02x ",
                    client->readBuffer[client->readOffset + i]);
        }
        fprintf(stderr, "\n");
#endif

        client->readOffset += bytesRead;

        // Parse message length if we're in that state
        if (client->state == CONN_READING_LENGTH) {
            DEBUG_LOG("Trying to parse varint length from %zu bytes\n",
                      client->readOffset);
            // Try to read varint length
            if (client->readOffset > 0) {
                uint64_t msgLen;
                size_t varintLen =
                    varintTaggedGet64(client->readBuffer, &msgLen);
                DEBUG_LOG("varintTaggedGet64 returned %zu, msgLen=%" PRIu64
                          "\n",
                          varintLen, msgLen);
                if (varintLen == 0) {
                    // Not enough bytes yet for complete varint
                    if (client->readOffset >= 9) {
                        // Invalid varint (too long)
                        disconnectClient(server, client);
                        return;
                    }
                    continue; // Try reading more data
                }

                client->messageLength = (size_t)msgLen;
                if (client->messageLength == 0 ||
                    client->messageLength > MAX_MESSAGE_SIZE) {
                    disconnectClient(server, client);
                    return;
                }

                // We have the length, move to reading message
                size_t lengthBytes = varintTaggedGetLen(client->readBuffer);
                client->messageBytesRead = client->readOffset - lengthBytes;

                // Move any extra bytes to beginning of buffer
                if (client->messageBytesRead > 0) {
                    memmove(client->readBuffer,
                            client->readBuffer + lengthBytes,
                            client->messageBytesRead);
                }
                client->readOffset = client->messageBytesRead;
                client->state = CONN_READING_MESSAGE;
            }
        }

        // Read message body
        if (client->state == CONN_READING_MESSAGE) {
            client->messageBytesRead = client->readOffset;

            if (client->messageBytesRead >= client->messageLength) {
                // Complete message received, process it
                processCommand(server, client, client->readBuffer,
                               client->messageLength);

                // Reset for next message
                // Only reset state if processCommand didn't change it (e.g., to
                // CONN_WRITING_RESPONSE)
                DEBUG_LOG("After processCommand, client state=%d\n",
                          client->state);
                if (client->state == CONN_READING_MESSAGE) {
                    DEBUG_LOG("State is still CONN_READING_MESSAGE, resetting "
                              "to CONN_READING_LENGTH\n");
                    size_t extraBytes =
                        client->messageBytesRead - client->messageLength;
                    if (extraBytes > 0) {
                        memmove(client->readBuffer,
                                client->readBuffer + client->messageLength,
                                extraBytes);
                    }
                    client->readOffset = extraBytes;
                    client->messageLength = 0;
                    client->messageBytesRead = 0;
                    client->state = CONN_READING_LENGTH;
                } else {
                    DEBUG_LOG("State changed to %d, breaking out of loop\n",
                              client->state);
                }
                // If state changed to CONN_WRITING_RESPONSE, exit loop to let
                // event loop handle it
                break;
            } else {
                // Need more data, continue reading
                continue;
            }
        }
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, const char *argv[]) {
    uint16_t port = DEFAULT_PORT;
    const char *authToken = NULL;
    const char *saveFile = NULL;

    printf("DEBUG: Starting trie_server\n");

    // Simple argument parsing
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--auth") == 0 && i + 1 < argc) {
            authToken = argv[++i];
        } else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            saveFile = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("Options:\n");
            printf("  --port <port>     Listen port (default: %d)\n",
                   DEFAULT_PORT);
            printf("  --auth <token>    Require authentication token\n");
            printf("  --save <file>     Auto-save file path\n");
            printf("  --help            Show this help\n");
            return 0;
        }
    }

    printf("DEBUG: Calling serverInit\n");
    fflush(stdout);

    // Allocate server on heap (struct is 16MB, too large for stack)
    TrieServer *server = (TrieServer *)malloc(sizeof(TrieServer));
    if (!server) {
        fprintf(stderr, "Failed to allocate server memory\n");
        return 1;
    }

    if (!serverInit(server, port, authToken, saveFile)) {
        fprintf(stderr, "Failed to initialize server\n");
        free(server);
        return 1;
    }

    printf("DEBUG: serverInit complete, calling serverRun\n");
    fflush(stdout);

    serverRun(server);
    serverShutdown(server);
    free(server);

    return 0;
}
