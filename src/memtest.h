#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Memtest - In-Process Memory Testing
 * ============================================================================
 * Memory testing patterns for detecting hardware memory errors:
 * - Address test: Each location stores its own address
 * - Random fill: Pseudo-random pattern using xorshift64*
 * - Solid fill: Alternating 0x00 and 0xFF
 * - Checkerboard: Alternating 0xAA and 0x55
 *
 * Based on Redis memtest by Salvatore Sanfilippo and Matt Stancliff.
 * ============================================================================
 */

/* ----------------------------------------------------------------------------
 * Test Results
 * --------------------------------------------------------------------------*/
typedef struct memtestResult {
    size_t bytes_tested;    /* Total bytes tested */
    size_t errors_found;    /* Number of errors detected */
    size_t passes_complete; /* Number of complete passes */
    double duration_s;      /* Test duration in seconds */
} memtestResult;

/* Progress callback: called periodically during testing
 * phase: test phase name (e.g., "Addressing", "Random fill")
 * progress: 0.0 to 1.0 */
typedef void (*memtestProgressFn)(const char *phase, double progress,
                                  void *userdata);

/* ----------------------------------------------------------------------------
 * Core Test Functions
 * --------------------------------------------------------------------------*/

/* Test memory region (non-destructive if preserving=true)
 * Returns number of errors detected (0 = no errors) */
size_t memtest(void *mem, size_t bytes, int passes, bool preserving);

/* Test with detailed results */
void memtestWithResult(void *mem, size_t bytes, int passes, bool preserving,
                       memtestResult *result);

/* Test with progress callback */
void memtestWithProgress(void *mem, size_t bytes, int passes, bool preserving,
                         memtestProgressFn progress_fn, void *userdata,
                         memtestResult *result);

/* ----------------------------------------------------------------------------
 * Individual Test Patterns (for advanced use)
 * --------------------------------------------------------------------------*/

/* Address test: fill each location with its own address, then verify */
int memtestAddressing(uint64_t *mem, size_t bytes);

/* Random fill: fill with pseudo-random pattern */
void memtestFillRandom(uint64_t *mem, size_t bytes);

/* Pattern fill: fill with alternating v1/v2 pattern */
void memtestFillPattern(uint64_t *mem, size_t bytes, uint64_t v1, uint64_t v2);

/* Compare: verify first half matches second half (returns error count) */
int memtestCompare(uint64_t *mem, size_t bytes);

/* ----------------------------------------------------------------------------
 * Convenience Functions
 * --------------------------------------------------------------------------*/

/* Allocate and test memory (returns error count) */
size_t memtestAllocAndTest(size_t megabytes, int passes, memtestResult *result);

/* Interactive test with terminal progress display
 * Note: This function will NOT call exit() - caller handles result */
void memtestInteractive(size_t megabytes, int passes, memtestResult *result);

/* ----------------------------------------------------------------------------
 * Process Memory Test (Cross-Platform)
 * --------------------------------------------------------------------------*/

/* Test all writable memory regions of current process
 * Uses non-destructive (preserving) mode to avoid data loss.
 * Returns count of errors detected.
 *
 * Platform support:
 * - Linux: /proc/self/maps
 * - macOS: Mach VM APIs
 * - FreeBSD/NetBSD/OpenBSD/DragonFly: /proc if mounted
 * - Other: Returns 0 (not supported)
 */
size_t memtestProcessMemory(int passes);

#ifdef DATAKIT_TEST
int memtestTest(int argc, char *argv[]);
#endif
