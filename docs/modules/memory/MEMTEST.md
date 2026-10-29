# memtest: testing RAM from inside a process

**Header**: `memtest.h` **Source**: `memtest.c`

memtest writes patterns through a memory region and reads them back to find
failing RAM: a crash report that points at memory corruption, a new server
being burned in, or a check before trusting a host. It finds hardware faults
that show up as flipped or stuck bits and address-line errors; it is not a
substitute for a boot-time memory tester, which can reach memory the process
cannot.

## Calls

```c
#include "memtest.h"

/* A fresh 512 MB allocation, 2 passes: the usual check */
memtestResult r;
const size_t errors = memtestAllocAndTest(512, 2, &r);
printf("%zu errors in %zu bytes (%.1f s)\n", r.errors_found, r.bytes_tested,
       r.duration_s);

/* The same with a terminal progress display (it never exits the process) */
memtestInteractive(512, 2, &r);

/* Every writable region of this process, contents preserved */
const size_t found = memtestProcessMemory(1);
```

| Call                                                              | Does                                                                                                                                                                                            |
| ----------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `memtest(mem, bytes, passes, preserving)`                         | test a region; the number of errors. With `preserving`, each chunk is saved and restored, so live data survives                                                                                 |
| `memtestWithResult(mem, bytes, passes, preserving, &r)`           | the same, filling `memtestResult` (bytes, errors, passes, seconds)                                                                                                                              |
| `memtestWithProgress(mem, bytes, passes, preserving, fn, ud, &r)` | the same, calling `fn(phase, progress 0..1, ud)` as it goes                                                                                                                                     |
| `memtestAllocAndTest(megabytes, passes, &r)`                      | allocate, test and free a region                                                                                                                                                                |
| `memtestInteractive(megabytes, passes, &r)`                       | `memtestAllocAndTest` with progress drawn on the terminal                                                                                                                                       |
| `memtestProcessMemory(passes)`                                    | every writable mapping of the process, preserving, except the calling thread's stack and the test's own buffer (Linux `/proc/self/maps`, the BSDs' `/proc`; macOS and others: 0, not supported) |

Each pass runs the patterns: every word holding its own address
(`memtestAddressing`, which catches address-line faults), a pseudo-random fill
(`memtestFillRandom`), solid 0x00/0xFF (`memtestFillPattern(mem, bytes, 0,
UINT64_MAX)`) and checkerboard 0xAA/0x55. The fills write the region's two
halves alike, and `memtestCompare(mem, bytes)` counts the words where they
differ. These are exported for tests of their own.

## Cautions

- Testing writes every byte: without `preserving`, the region's contents are
  gone. `memtestProcessMemory` always preserves, but other threads must not
  touch memory while it runs (stop them first).
- A preserving test saves each chunk in a 1 MB heap buffer, so it runs on
  threads with small stacks; a region needs at least 8 KB, in whole 4 KB
  pages, or nothing is tested (`bytes_tested` 0).
- A test only reaches the physical pages behind the region, and the kernel
  decides which those are; a large allocation and several passes cover more.
- A clean run does not prove the RAM sound: intermittent faults may need
  heat, load or many passes to appear.
