#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* membound - Bounded Memory Pool Allocator
 *
 * A buddy allocation system for managing a pre-allocated memory pool.
 * Useful for:
 *   - Per-task/per-request memory isolation
 *   - Bounded memory usage (won't exceed pool size)
 *   - Fast bulk deallocation via memboundReset()
 *   - Process-shared memory (works across fork)
 *
 * Thread safety: All functions are thread-safe via internal mutex.
 * Exception: Shutdown functions require no concurrent usage.
 */

typedef struct membound membound;

/* === Creation and Destruction === */

/* Create a new memory pool of the specified size.
 * Returns NULL on failure (e.g., mmap failed). */
membound *memboundCreate(size_t size);

/* Destroy the pool, releasing all memory.
 * WARNING: Invalidates all pointers from this pool! */
bool memboundShutdown(membound *m);

/* Destroy the pool only if no allocations are outstanding.
 * Returns false if allocations exist (pool not destroyed). */
bool memboundShutdownSafe(membound *m);

/* === Allocation Functions === */

/* Allocate memory from the pool. Returns NULL if exhausted. */
void *memboundAlloc(membound *m, size_t size);

/* Allocate zero-initialized memory (like calloc). */
void *memboundCalloc(membound *m, size_t count, size_t size);

/* Resize an allocation. Returns NULL on failure (original preserved). */
void *memboundRealloc(membound *m, void *p, size_t newlen);

/* Free memory back to the pool. NULL pointers are safely ignored. */
void memboundFree(membound *m, void *p);

/* === Pool Management === */

/* Reset the pool, freeing all allocations at once.
 * WARNING: Invalidates all pointers from this pool! */
void memboundReset(membound *m);

/* Grow the pool to a new size. Only works when pool is empty.
 * Returns false if allocations exist or growth fails. */
bool memboundIncreaseSize(membound *m, size_t size);

/* === Statistics and Queries === */

/* Get the number of outstanding allocations. */
size_t memboundCurrentAllocationCount(const membound *m);

/* Get total bytes currently allocated (includes internal fragmentation). */
size_t memboundBytesUsed(const membound *m);

/* Get approximate bytes available for new allocations. */
size_t memboundBytesAvailable(const membound *m);

/* Get total pool capacity in usable bytes. */
size_t memboundCapacity(const membound *m);

/* Check if a pointer belongs to this pool's memory region. */
bool memboundOwns(const membound *m, const void *p);

#ifdef DATAKIT_TEST
int memboundTest(int argc, char *argv[]);
#endif
