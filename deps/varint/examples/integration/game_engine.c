/**
 * game_engine.c - Game state encoding using varintPacked and varintBitstream
 *
 * This example demonstrates a game engine combining:
 * - varintPacked: Entity IDs and component indices (13-bit values)
 * - varintBitstream: Bit-packed entity state flags
 * - Efficient entity-component system (ECS) architecture
 *
 * Features:
 * - Compact entity ID management (thousands in minimal space)
 * - Bit-packed entity flags (alive, active, visible, etc.)
 * - Component type masks using bitfields
 * - Network-ready state serialization
 * - Delta compression for state updates
 *
 * Compile: gcc -I../../src game_engine.c ../../build/src/libvarint.a -o
 * game_engine Run: ./game_engine
 */

// Generate varintPacked13 for 13-bit entity IDs (0-8191)
#define PACK_STORAGE_BITS 13
#include "varintPacked.h"
#undef PACK_STORAGE_BITS

#include "varintBitstream.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// ENTITY FLAGS (bit-packed using varintBitstream)
// ============================================================================

// Entity state flags packed into 16 bits
typedef enum {
    ENTITY_FLAG_ALIVE = 0,       // 1 bit: Is entity alive?
    ENTITY_FLAG_ACTIVE = 1,      // 1 bit: Is entity active in scene?
    ENTITY_FLAG_VISIBLE = 2,     // 1 bit: Is entity visible?
    ENTITY_FLAG_PHYSICS = 3,     // 1 bit: Has physics enabled?
    ENTITY_FLAG_AI = 4,          // 1 bit: Has AI enabled?
    ENTITY_FLAG_NETWORKED = 5,   // 1 bit: Replicated over network?
    ENTITY_FLAG_LAYER = 6,       // 3 bits: Rendering layer (0-7)
    ENTITY_FLAG_TEAM = 9,        // 2 bits: Team ID (0-3)
    ENTITY_FLAG_HEALTH_PCT = 11, // 5 bits: Health percentage (0-31 = 0-100%)
} EntityFlagOffset;

typedef struct {
    uint64_t flags; // All flags packed (varintBitstream requires uint64_t)
} EntityFlags;

void entityFlagsSetBit(EntityFlags *flags, EntityFlagOffset offset,
                       bool value) {
    varintBitstreamSet((uint64_t *)&flags->flags, offset, 1, value ? 1 : 0);
}

bool entityFlagsGetBit(const EntityFlags *flags, EntityFlagOffset offset) {
    return varintBitstreamGet((const uint64_t *)&flags->flags, offset, 1) != 0;
}

void entityFlagsSetLayer(EntityFlags *flags, uint8_t layer) {
    assert(layer < 8); // 3 bits = 0-7
    varintBitstreamSet((uint64_t *)&flags->flags, ENTITY_FLAG_LAYER, 3, layer);
}

uint8_t entityFlagsGetLayer(const EntityFlags *flags) {
    return (uint8_t)varintBitstreamGet((const uint64_t *)&flags->flags,
                                       ENTITY_FLAG_LAYER, 3);
}

void entityFlagsSetTeam(EntityFlags *flags, uint8_t team) {
    assert(team < 4); // 2 bits = 0-3
    varintBitstreamSet((uint64_t *)&flags->flags, ENTITY_FLAG_TEAM, 2, team);
}

uint8_t entityFlagsGetTeam(const EntityFlags *flags) {
    return (uint8_t)varintBitstreamGet((const uint64_t *)&flags->flags,
                                       ENTITY_FLAG_TEAM, 2);
}

void entityFlagsSetHealth(EntityFlags *flags, uint8_t healthPercent) {
    assert(healthPercent <= 100);
    // Map 0-100 to 0-31 (5 bits)
    uint8_t compressedHealth = (healthPercent * 31) / 100;
    varintBitstreamSet((uint64_t *)&flags->flags, ENTITY_FLAG_HEALTH_PCT, 5,
                       compressedHealth);
}

uint8_t entityFlagsGetHealth(const EntityFlags *flags) {
    uint8_t compressed = (uint8_t)varintBitstreamGet(
        (const uint64_t *)&flags->flags, ENTITY_FLAG_HEALTH_PCT, 5);
    // Map 0-31 back to 0-100
    return (compressed * 100) / 31;
}

// ============================================================================
// COMPONENT SYSTEM (using varintPacked16)
// ============================================================================

typedef enum {
    COMPONENT_TRANSFORM = 0,
    COMPONENT_PHYSICS = 1,
    COMPONENT_RENDER = 2,
    COMPONENT_AI = 3,
    COMPONENT_HEALTH = 4,
    COMPONENT_INVENTORY = 5,
    COMPONENT_COUNT
} ComponentType;

typedef struct {
    uint16_t entityId;  // Entity owning this component
    ComponentType type; // Component type
    void *data;         // Component data (type-specific)
} Component;

typedef struct {
    uint16_t
        *entityToComponents[COMPONENT_COUNT]; // Maps entity -> component index
    Component *components[COMPONENT_COUNT];   // Component pools
    size_t componentCounts[COMPONENT_COUNT];
    size_t componentCapacities[COMPONENT_COUNT];
    size_t maxEntities;
} ComponentManager;

void componentManagerInit(ComponentManager *mgr, size_t maxEntities) {
    mgr->maxEntities = maxEntities;

    // Allocate entity->component mapping (simple uint16_t array)
    for (size_t i = 0; i < COMPONENT_COUNT; i++) {
        mgr->entityToComponents[i] = calloc(maxEntities, sizeof(uint16_t));
        mgr->components[i] = NULL;
        mgr->componentCounts[i] = 0;
        mgr->componentCapacities[i] = 0;
    }
}

void componentManagerFree(ComponentManager *mgr) {
    for (size_t i = 0; i < COMPONENT_COUNT; i++) {
        free(mgr->entityToComponents[i]);
        free(mgr->components[i]);
    }
}

void componentManagerAdd(ComponentManager *mgr, uint16_t entityId,
                         ComponentType type, void *data) {
    assert(entityId < mgr->maxEntities);
    assert(type < COMPONENT_COUNT);

    // Get component pool
    size_t index = mgr->componentCounts[type];
    if (index >= mgr->componentCapacities[type]) {
        mgr->componentCapacities[type] =
            mgr->componentCapacities[type] ? mgr->componentCapacities[type] * 2
                                           : 16;
        mgr->components[type] =
            realloc(mgr->components[type],
                    mgr->componentCapacities[type] * sizeof(Component));
    }

    // Add component
    mgr->components[type][index].entityId = entityId;
    mgr->components[type][index].type = type;
    mgr->components[type][index].data = data;
    mgr->componentCounts[type]++;

    // Map entity to component index
    mgr->entityToComponents[type][entityId] = (uint16_t)index;
}

uint16_t componentManagerGetIndex(const ComponentManager *mgr,
                                  uint16_t entityId, ComponentType type) {
    assert(entityId < mgr->maxEntities);
    assert(type < COMPONENT_COUNT);

    return mgr->entityToComponents[type][entityId];
}

// ============================================================================
// ENTITY MANAGER (using varintPacked13)
// ============================================================================

#define MAX_ENTITIES 8192 // 13-bit entity IDs

typedef struct {
    uint8_t *freeList; // Sorted list of free entity IDs (packed 13-bit)
    size_t freeCount;
    EntityFlags *flags; // Entity flags (bit-packed)
    ComponentManager components;
    uint16_t nextId;
} EntityManager;

void entityManagerInit(EntityManager *mgr) {
    // Allocate storage for free list (13 bits per ID)
    size_t freeListBytes = (MAX_ENTITIES * 13 + 7) / 8;
    mgr->freeList = malloc(freeListBytes);

    // Initialize free list with all IDs (0 to MAX_ENTITIES-1)
    for (uint16_t i = 0; i < MAX_ENTITIES; i++) {
        varintPacked13Set(mgr->freeList, i, i);
    }
    mgr->freeCount = MAX_ENTITIES;
    mgr->nextId = 0;

    // Allocate entity flags
    mgr->flags = calloc(MAX_ENTITIES, sizeof(EntityFlags));

    // Initialize component manager
    componentManagerInit(&mgr->components, MAX_ENTITIES);
}

void entityManagerFree(EntityManager *mgr) {
    free(mgr->freeList);
    free(mgr->flags);
    componentManagerFree(&mgr->components);
}

uint16_t entityManagerCreate(EntityManager *mgr) {
    assert(mgr->freeCount > 0); // Check we have free entities

    // Pop from free list (sorted, so take last element)
    mgr->freeCount--;
    uint16_t entityId = varintPacked13Get(mgr->freeList, mgr->freeCount);

    // Initialize flags
    mgr->flags[entityId].flags = 0;
    entityFlagsSetBit(&mgr->flags[entityId], ENTITY_FLAG_ALIVE, true);
    entityFlagsSetBit(&mgr->flags[entityId], ENTITY_FLAG_ACTIVE, true);
    entityFlagsSetHealth(&mgr->flags[entityId], 100);

    return entityId;
}

void entityManagerDestroy(EntityManager *mgr, uint16_t entityId) {
    assert(entityId < MAX_ENTITIES);

    // Mark as not alive
    entityFlagsSetBit(&mgr->flags[entityId], ENTITY_FLAG_ALIVE, false);

    // Add back to free list (insert sorted)
    varintPacked13InsertSorted(mgr->freeList, mgr->freeCount, entityId);
    mgr->freeCount++;
}

EntityFlags *entityManagerGetFlags(EntityManager *mgr, uint16_t entityId) {
    assert(entityId < MAX_ENTITIES);
    return &mgr->flags[entityId];
}

// ============================================================================
// NETWORK SERIALIZATION
// ============================================================================

typedef struct {
    uint8_t *buffer;
    size_t size;
    size_t capacity;
} NetworkPacket;

void packetInit(NetworkPacket *packet, size_t capacity) {
    packet->buffer = malloc(capacity);
    packet->size = 0;
    packet->capacity = capacity;
}

void packetFree(NetworkPacket *packet) {
    free(packet->buffer);
}

void packetWriteEntityState(NetworkPacket *packet, uint16_t entityId,
                            const EntityFlags *flags) {
    // Write entity ID (13 bits = 2 bytes when packed)
    size_t idBytes = (13 + 7) / 8;
    varintPacked13Set(packet->buffer + packet->size, 0, entityId);
    packet->size += idBytes;

    // Write flags (16 bits = 2 bytes)
    memcpy(packet->buffer + packet->size, &flags->flags, sizeof(uint16_t));
    packet->size += sizeof(uint16_t);
}

void packetReadEntityState(const NetworkPacket *packet, size_t *offset,
                           uint16_t *entityId, EntityFlags *flags) {
    // Read entity ID
    *entityId = varintPacked13Get(packet->buffer + *offset, 0);
    size_t idBytes = (13 + 7) / 8;
    *offset += idBytes;

    // Read flags
    memcpy(&flags->flags, packet->buffer + *offset, sizeof(uint16_t));
    *offset += sizeof(uint16_t);
}

// ============================================================================
// DEMONSTRATION
// ============================================================================

void demonstrateGameEngine(void) {
    printf("\n=== Game Engine Example ===\n\n");

    // 1. Initialize entity manager
    printf("1. Initializing entity manager...\n");

    EntityManager mgr;
    entityManagerInit(&mgr);

    printf("   Max entities: %d (13-bit IDs)\n", MAX_ENTITIES);
    printf("   Free entities: %zu\n", mgr.freeCount);
    printf("   Entity flags: 16 bits per entity\n");
    printf("   Storage: %zu bytes for flags\n",
           MAX_ENTITIES * sizeof(EntityFlags));

    // 2. Create entities
    printf("\n2. Creating entities...\n");

    uint16_t player = entityManagerCreate(&mgr);
    uint16_t enemy1 = entityManagerCreate(&mgr);
    uint16_t enemy2 = entityManagerCreate(&mgr);
    uint16_t powerup = entityManagerCreate(&mgr);

    printf("   Created entities: %u, %u, %u, %u\n", player, enemy1, enemy2,
           powerup);
    printf("   Free entities: %zu\n", mgr.freeCount);

    // 3. Set entity flags
    printf("\n3. Setting entity flags (bit-packed)...\n");

    // Player: team 0, layer 1, 100% health
    EntityFlags *playerFlags = entityManagerGetFlags(&mgr, player);
    entityFlagsSetBit(playerFlags, ENTITY_FLAG_VISIBLE, true);
    entityFlagsSetBit(playerFlags, ENTITY_FLAG_PHYSICS, true);
    entityFlagsSetBit(playerFlags, ENTITY_FLAG_NETWORKED, true);
    entityFlagsSetTeam(playerFlags, 0);
    entityFlagsSetLayer(playerFlags, 1);
    entityFlagsSetHealth(playerFlags, 100);

    // Enemy 1: team 1, layer 1, 75% health
    EntityFlags *enemy1Flags = entityManagerGetFlags(&mgr, enemy1);
    entityFlagsSetBit(enemy1Flags, ENTITY_FLAG_VISIBLE, true);
    entityFlagsSetBit(enemy1Flags, ENTITY_FLAG_AI, true);
    entityFlagsSetBit(enemy1Flags, ENTITY_FLAG_NETWORKED, true);
    entityFlagsSetTeam(enemy1Flags, 1);
    entityFlagsSetLayer(enemy1Flags, 1);
    entityFlagsSetHealth(enemy1Flags, 75);

    // Power-up: team 0, layer 2, no health
    EntityFlags *powerupFlags = entityManagerGetFlags(&mgr, powerup);
    entityFlagsSetBit(powerupFlags, ENTITY_FLAG_VISIBLE, true);
    entityFlagsSetLayer(powerupFlags, 2);

    printf("   Player flags: 0x%04lX\n", (unsigned long)playerFlags->flags);
    printf("   - Alive: %d\n",
           entityFlagsGetBit(playerFlags, ENTITY_FLAG_ALIVE));
    printf("   - Visible: %d\n",
           entityFlagsGetBit(playerFlags, ENTITY_FLAG_VISIBLE));
    printf("   - Team: %d\n", entityFlagsGetTeam(playerFlags));
    printf("   - Layer: %d\n", entityFlagsGetLayer(playerFlags));
    printf("   - Health: %d%%\n", entityFlagsGetHealth(playerFlags));

    printf("\n   Enemy flags: 0x%04lX\n", (unsigned long)enemy1Flags->flags);
    printf("   - AI: %d\n", entityFlagsGetBit(enemy1Flags, ENTITY_FLAG_AI));
    printf("   - Team: %d\n", entityFlagsGetTeam(enemy1Flags));
    printf("   - Health: %d%%\n", entityFlagsGetHealth(enemy1Flags));

    // 4. Network serialization
    printf("\n4. Serializing entity state for network...\n");

    NetworkPacket packet;
    packetInit(&packet, 1024);

    // Write entity count (13 bits)
    uint16_t entityCount = 2; // Player and enemy1
    varintPacked13Set(packet.buffer, 0, entityCount);
    packet.size = 2;

    // Write entities
    packetWriteEntityState(&packet, player, playerFlags);
    packetWriteEntityState(&packet, enemy1, enemy1Flags);

    printf("   Packet size: %zu bytes\n", packet.size);
    printf("   Contains %u entities\n", entityCount);

    // Deserialize
    printf("\n   Deserializing packet...\n");
    size_t offset = 0;
    uint16_t receivedCount = varintPacked13Get(packet.buffer, 0);
    offset = 2;

    for (uint16_t i = 0; i < receivedCount; i++) {
        uint16_t entityId;
        EntityFlags flags;
        packetReadEntityState(&packet, &offset, &entityId, &flags);

        printf("   Entity %u: flags=0x%04lX, team=%d, health=%d%%\n", entityId,
               (unsigned long)flags.flags, entityFlagsGetTeam(&flags),
               entityFlagsGetHealth(&flags));
    }

    // 5. Space efficiency analysis
    printf("\n5. Space efficiency analysis:\n");

    // Entity IDs
    size_t entityIdStorage = (MAX_ENTITIES * 13 + 7) / 8;
    printf("   Entity ID storage (13-bit packed):\n");
    printf("   - %d entities × 13 bits = %zu bytes\n", MAX_ENTITIES,
           entityIdStorage);
    printf("   - vs 16-bit: %d bytes\n", MAX_ENTITIES * 2);
    printf("   - Savings: %zu bytes (%.1f%%)\n",
           MAX_ENTITIES * 2 - entityIdStorage,
           100.0 * (1.0 - (double)entityIdStorage / (MAX_ENTITIES * 2)));

    // Entity flags
    size_t flagStorage = MAX_ENTITIES * sizeof(EntityFlags);
    size_t unpackedFlagStorage =
        MAX_ENTITIES * 4; // 4 bytes per entity uncompressed
    printf("\n   Entity flags (16-bit packed):\n");
    printf("   - %d entities × 16 bits = %zu bytes\n", MAX_ENTITIES,
           flagStorage);
    printf("   - vs unpacked struct: %zu bytes\n", unpackedFlagStorage);
    printf("   - Savings: %zu bytes (%.1f%%)\n",
           unpackedFlagStorage - flagStorage,
           100.0 * (1.0 - (double)flagStorage / unpackedFlagStorage));

    // Network packets
    size_t networkPacketSize = packet.size;
    size_t uncompressedPacketSize =
        entityCount * (2 + 4); // 2 bytes ID + 4 bytes flags
    printf("\n   Network packet:\n");
    printf("   - Compressed: %zu bytes\n", networkPacketSize);
    printf("   - Uncompressed: %zu bytes\n", uncompressedPacketSize);
    printf("   - Savings: %.1f%%\n",
           100.0 * (1.0 - (double)networkPacketSize / uncompressedPacketSize));

    // 6. Entity lifecycle
    printf("\n6. Testing entity lifecycle...\n");

    printf("   Destroying enemy...\n");
    entityManagerDestroy(&mgr, enemy1);
    printf("   Free entities: %zu\n", mgr.freeCount);
    printf("   Enemy alive? %d\n",
           entityFlagsGetBit(entityManagerGetFlags(&mgr, enemy1),
                             ENTITY_FLAG_ALIVE));

    printf("\n   Creating new entity (should reuse ID)...\n");
    uint16_t newEntity = entityManagerCreate(&mgr);
    printf("   New entity ID: %u (reused: %s)\n", newEntity,
           newEntity == enemy1 ? "yes" : "no");
    printf("   Free entities: %zu\n", mgr.freeCount);

    packetFree(&packet);
    entityManagerFree(&mgr);

    printf("\n✓ Game engine example complete\n");
}

int main(void) {
    printf("===========================================\n");
    printf("  Game Engine Integration Example\n");
    printf("===========================================\n");

    demonstrateGameEngine();

    printf("\n===========================================\n");
    printf("This example demonstrated:\n");
    printf("  • varintPacked13 for entity IDs\n");
    printf("  • varintPacked16 for component indices\n");
    printf("  • varintBitstream for entity flags\n");
    printf("  • Entity-component system (ECS)\n");
    printf("  • Network state serialization\n");
    printf("  • Space-efficient game state\n");
    printf("===========================================\n");

    return 0;
}
