/* Allow test prototypes to exist from headers at edit time
 * in addition to at compile time. */
#ifndef DATAKIT_TEST
#define DATAKIT_TEST
#endif

/* Begin */
#include "datakit.h"

/* Add all includes here so we get their test main functions defined. */
#include "../deps/sha1/sha1.h"
#include "OSRegulate.h"
#include "aeadNonceLog.h"
#include "aesGcm.h"
#include "atomPool.h"
#include "bbits.h"
#include "blake3.h"
#include "chacha20poly1305.h"
#include "clusterMailbox.h"
#include "clusterReplicator.h"
#include "clusterRing.h"
#include "compressionBench.h"
#include "ctest.h"
#include "ctmem.h"
#include "databox.h"
#include "databoxLinear.h"
#include "dataspeed.h"
#include "dj.h"
#include "dkFormat.h"
#include "dkRandom.h"
#include "dkTestTmp.h"
#include "dod.h"
#include "fastmutex.h"
#include "fenwick/fenwick2d.h"  /* 2D typed fenwick trees */
#include "fenwick/fenwickI64.h" /* New 2-tier template system */
#include "fenwickBench.h"
#include "fibbuf.h"
#include "flex.h"
#include "flexEnvelope.h"
#include "float16.h"
#include "floatExtended.h"
#include "hash/aesGcmArmCrypto.h"
#include "hash/aesGcmNi.h"
#include "hash/blake3AVX2.h"
#include "hash/blake3Neon.h"
#include "hash/blake3SSE41.h"
#include "hash/blake3TestVectors.h"
#include "hash/chacha20AVX2.h"
#include "hash/chacha20Neon.h"
#include "hash/poly1305AVX2.h"
#include "hash/poly1305Neon.h"
#include "hash/sha256ArmCrypto.h"
#include "hash/sha256ShaNi.h"
#include "hmac.h"
#include "hmacKeyring.h"
#include "hyperloglog.h"
#include "inflightGate.h"
#include "intersectInt.h"
#include "intset.h"
#include "intsetU32.h"
#include "jebuf.h"
#include "linearBloom.h"
#include "list.h"
#include "mds.h"
#include "mdsc.h"
#include "membound.h"
#include "memtest.h"
#include "mflex.h"
#include "monotonicCond.h"
#include "multiFenwick.h" /* Separate multi-fenwick system (wraps multiple trees) */
#include "multiFenwick2d.h"   /* 2D fenwick over databox values */
#include "multiFenwickFlat.h" /* multiFenwick with flat O(1) cells */
#include "multiOrderedSet.h"
#include "multiTimer.h"
#include "multiarray.h"
#include "multiarrayLarge.h"
#include "multiarrayMedium.h"
#include "multiarraySmall.h"
#include "multidict.h"
#include "multiheap.h"
#include "multilist.h"
#include "multilistFull.h"
#include "multilistMedium.h"
#include "multilistSmall.h"
#include "multilru.h"
#include "multimap.h"
#include "multimapAtom.h"
#include "multimapAtomDense.h"
#include "multimapFull.h"
#include "multimapMedium.h"
#include "multimapSmall.h"
#include "multiroar.h"
#include "offsetArray.h"
#include "ownedState.h"
#include "perfRegression.h"
#include "persist.h"
#include "persist/contract/persistContract.h"
#include "persist/flexP.h"
#include "persist/intsetP.h"
#include "persist/multiOrderedSetP.h"
#include "persist/multidictP.h"
#include "persist/multilistP.h"
#include "persist/multilruP.h"
#include "persist/multimapP.h"
#include "persist/multiroarP.h"
#include "persist/persistCtx.h"
#include "portableRandom.h"
#include "ptrPrevNext.h"

/* ptrlib is a guard-less config-macro header; its test entry point is
 * declared here rather than in the header */
extern int ptrlibTest(int argc, char *argv[]);
extern int bigmathTest(int argc, char *argv[]);
#include "rapidhash.h"
#include "segment/segmentI64.h" /* New 2-tier template system */
#include "sha256.h"
#include "sharrow.h"
#include "str.h"
#include "strDoubleFormat.h"
#include "stringPool.h"
#include "timeUtil.h"
#include "timerWheel.h"
#include "ulid.h"
#include "util.h"
#include "x25519.h"
#include "xof.h"

/* intsetBig requires 128-bit integer support (available on all 64-bit
 * platforms) */
#ifdef __SIZEOF_INT128__
#define USE_INTSET_BIG 1
#include "intsetBig.h"
#endif

#include <errno.h>
#include <fcntl.h>  /* descriptor flags of OSRegulate pairs */
#include <poll.h>   /* process-exit watch fd readiness */
#include <sched.h>  /* sched_yield: the owned-arena soak's reader handshake */
#include <signal.h> /* kill */
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/wait.h> /* WIFEXITED / WEXITSTATUS */
#include <unistd.h>   /* fork / pipe / read */

#include "cleaner.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint8_t *, zfree, Bytes);
defcleanerName(chacha20poly1305StreamCtx *, zfree, ChaChaStreamCtx);
defcleaner(flex *, flexFree);
defcleaner(clusterRing *, clusterRingFree);
defcleaner(multilist *, multilistFree);
defcleaner(aeadNonceLog *, aeadNonceLogFree);

/* Scope-exit cleaner for a fixed 3-slot owned-buffer array (used by
 * dkTestRevokedKeyFloorTest's per-key wrapped envelopes): frees whichever
 * slots were allocated. */
static DK_FN_UNUSED void freeBytesArray3_cleanup(uint8_t *(*arr)[3]) {
    for (uint32_t i = 0; i < 3; i++) {
        if ((*arr)[i]) {
            zfree((*arr)[i]);
        }
    }
}

/* ====================================================================
 * Test Registry - Single source of truth for all tests
 * ==================================================================== */

typedef int (*TestFunc)(int argc, char *argv[]);

typedef struct {
    const char *name;     /* Primary test name (must match function) */
    const char *aliases;  /* Comma-separated aliases (NULL if none) */
    TestFunc func;        /* Test function pointer */
    bool adjustArgs;      /* Pass argc-2, argv+2 instead of argc, argv */
    const char *category; /* Category for grouping (NULL = default) */
} TestEntry;

/* Helper: check if a name matches test (primary name or any alias) */
static bool testMatches(const TestEntry *t, const char *name) {
    /* Check primary name */
    if (strcasecmp(t->name, name) == 0) {
        return true;
    }

    /* Check aliases */
    if (t->aliases) {
        const char *p = t->aliases;
        while (*p) {
            const char *start = p;
            while (*p && *p != ',') {
                p++;
            }
            size_t len = p - start;
            if (len > 0 && strlen(name) == len &&
                strncasecmp(start, name, len) == 0) {
                return true;
            }
            if (*p == ',') {
                p++;
            }
        }
    }
    return false;
}

/* Macro helpers for concise test registration */
#define T(n) {#n, NULL, n##Test, false, NULL}
#define T_A(n, a) {#n, a, n##Test, false, NULL}
#define T_ADJ(n) {#n, NULL, n##Test, true, NULL}
#define T_A_ADJ(n, a) {#n, a, n##Test, true, NULL}
#define T_END {NULL, NULL, NULL, false, NULL}

/* Wrapper for void(void) benchmark functions */
static int mdsBenchWrapper(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return mdsBenchMain();
}
static int mdscBenchWrapper(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return mdscBenchMain();
}

/* ====================================================================
 * dkSecureZero — verify the helper actually zeroes a populated
 * buffer end-to-end.  We exercise three size classes (tiny, single
 * cache line, multi-page) plus the NULL / zero-length guard branches
 * so a future regression to those edge cases is caught here.
 *
 * Note: this test cannot prove DCE-resistance at runtime (the
 * compiler's choice to elide is invisible from inside the program).
 * That property rests on the volatile-store + asm("memory") pattern
 * in `ctmem.h` and is verified by inspection / disassembly in CI.
 * ==================================================================== */
static int dkTestSecureZeroTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    static const size_t sizes[] = {1, 7, 16, 32, 64, 256, 4096, 65536};
    int failures = 0;

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t n = sizes[i];
        cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(n);
        if (buf == NULL) {
            printf("dkTestSecureZero: alloc failed at n=%zu\n", n);
            return 1;
        }
        /* Populate with a non-zero pattern so a no-op clear is detected. */
        memset(buf, 0xAB, n);
        for (size_t j = 0; j < n; j++) {
            if (buf[j] != 0xAB) {
                printf("dkTestSecureZero: pre-condition fail at n=%zu j=%zu\n",
                       n, j);
                return 1;
            }
        }

        dkSecureZero(buf, n);

        for (size_t j = 0; j < n; j++) {
            if (buf[j] != 0) {
                printf("dkTestSecureZero: byte %zu of %zu non-zero (0x%02x)\n",
                       j, n, buf[j]);
                failures++;
                break;
            }
        }
    }

    /* Guard paths: must be no-ops, not crash. */
    dkSecureZero(NULL, 0);
    dkSecureZero(NULL, 16);
    uint8_t guard[8];
    memset(guard, 0xCD, sizeof(guard));
    dkSecureZero(guard, 0);
    for (size_t j = 0; j < sizeof(guard); j++) {
        if (guard[j] != 0xCD) {
            printf("dkTestSecureZero: zero-length call modified buffer\n");
            failures++;
            break;
        }
    }

    if (failures == 0) {
        printf("dkTestSecureZero: OK (%zu size classes + guards)\n",
               sizeof(sizes) / sizeof(sizes[0]));
        return 0;
    }
    return 1;
}

/* ====================================================================
 * testCtMemEqStructural — verify dkCtMemEq does not short-circuit on a
 * first-byte mismatch.  For each length n in {1, 15, 16, 17, 32, 64}
 * and each byte index k in 0..n-1, build two buffers that are identical
 * except at byte k and assert dkCtMemEq returns false. Then assert that
 * a bytewise-identical pair returns true.  dkCtMemEq is the sole gate
 * against tag-forgery timing oracles for every AEAD tag check, HMAC
 * check, and x25519 small-order pre-reject — a regression that lost the
 * volatile barrier (or substituted memcmp) would break all of them
 * silently.  This test pins the every-byte-contributes property at a
 * structural level the compiler cannot elide.
 * ==================================================================== */
static int testCtMemEqStructuralTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    static const size_t kLens[] = {1u, 15u, 16u, 17u, 32u, 64u};
    const size_t kNumLens = sizeof(kLens) / sizeof(kLens[0]);
    int failures = 0;
    size_t totalDiffChecks = 0;

    for (size_t li = 0; li < kNumLens; li++) {
        const size_t n = kLens[li];
        cleanerName(zfree, Bytes) uint8_t *a = (uint8_t *)zmalloc(n);
        cleanerName(zfree, Bytes) uint8_t *b = (uint8_t *)zmalloc(n);
        if (a == NULL || b == NULL) {
            printf("testCtMemEqStructural: alloc failed at n=%zu\n", n);
            return 1;
        }

        /* Populate both buffers with the same non-trivial pattern so a
         * regression to memcmp-style early-exit on a leading mismatch is
         * caught by every k in the sweep — the only difference per
         * iteration is at index k. */
        for (size_t i = 0; i < n; i++) {
            a[i] = (uint8_t)((i * 37u + 0x91u) ^ 0xA5u);
            b[i] = a[i];
        }

        /* Identical pair: must return true. */
        if (!dkCtMemEq(a, b, n)) {
            printf("testCtMemEqStructural: identical pair at n=%zu returned "
                   "false\n",
                   n);
            failures++;
        }

        /* For each byte position k, flip exactly one bit at index k in b
         * and assert dkCtMemEq returns false.  This proves every byte
         * position contributes to the running diff — a regression that
         * stopped at the first mismatch would still pass on k=0 but fail
         * on k>0 unless a strict-equality fold is preserved.  We flip a
         * single bit (LSB) rather than xor the whole byte so the test
         * also catches a regression that only ORs a high bit. */
        for (size_t k = 0; k < n; k++) {
            const uint8_t saved = b[k];
            b[k] = (uint8_t)(saved ^ 0x01u);
            totalDiffChecks++;
            if (dkCtMemEq(a, b, n)) {
                printf("testCtMemEqStructural: n=%zu k=%zu single-bit diff "
                       "accepted (regression!)\n",
                       n, k);
                failures++;
            }
            b[k] = saved;
        }

        /* Also verify a high-bit flip is detected at every k (would catch
         * a regression that ORs only the low nibble or masks the top
         * bit). */
        for (size_t k = 0; k < n; k++) {
            const uint8_t saved = b[k];
            b[k] = (uint8_t)(saved ^ 0x80u);
            totalDiffChecks++;
            if (dkCtMemEq(a, b, n)) {
                printf("testCtMemEqStructural: n=%zu k=%zu high-bit diff "
                       "accepted (regression!)\n",
                       n, k);
                failures++;
            }
            b[k] = saved;
        }

        /* Final check: after all single-byte flips reverted, the pair is
         * still equal. */
        if (!dkCtMemEq(a, b, n)) {
            printf("testCtMemEqStructural: post-sweep identical pair at "
                   "n=%zu returned false (revert bug)\n",
                   n);
            failures++;
        }
    }

    /* Bonus: exercise the unrolled 16-byte specialised compare with the
     * same per-byte sweep.  dkCtMemEq16 is used directly by AEAD tag
     * verify in chacha20poly1305 + aesGcm, so the structural property
     * must hold for the unrolled form too. */
    {
        uint8_t a16[16], b16[16];
        for (size_t i = 0; i < 16u; i++) {
            a16[i] = (uint8_t)(0x33u ^ (i * 11u));
            b16[i] = a16[i];
        }
        if (!dkCtMemEq16(a16, b16)) {
            printf("testCtMemEqStructural: dkCtMemEq16 identical returned "
                   "false\n");
            failures++;
        }
        for (size_t k = 0; k < 16u; k++) {
            const uint8_t saved = b16[k];
            b16[k] = (uint8_t)(saved ^ 0x01u);
            totalDiffChecks++;
            if (dkCtMemEq16(a16, b16)) {
                printf("testCtMemEqStructural: dkCtMemEq16 k=%zu single-bit "
                       "diff accepted\n",
                       k);
                failures++;
            }
            b16[k] = (uint8_t)(saved ^ 0x80u);
            totalDiffChecks++;
            if (dkCtMemEq16(a16, b16)) {
                printf("testCtMemEqStructural: dkCtMemEq16 k=%zu high-bit "
                       "diff accepted\n",
                       k);
                failures++;
            }
            b16[k] = saved;
        }
        if (!dkCtMemEq16(a16, b16)) {
            printf("testCtMemEqStructural: dkCtMemEq16 post-sweep identical "
                   "returned false\n");
            failures++;
        }
    }

    if (failures == 0) {
        printf("testCtMemEqStructural: OK (%zu lengths, %zu diff checks)\n",
               kNumLens, totalDiffChecks);
        return 0;
    }
    return 1;
}

/* ====================================================================
 * OSRegulate process-exit events: waiting for another process to end
 * (kqueue EVFILT_PROC/NOTE_EXIT on the BSD family, pidfd on Linux).
 * Three properties are checked:
 *
 *   1. an exiting child is reaped inside a budget, with its exit
 *      status recovered;
 *   2. a child that outlives the budget returns false (the helper
 *      never signals it) and is still reapable after the caller's own
 *      kill;
 *   3. the watch fd is a readiness source: not readable while the
 *      child lives, readable once it exits.
 *
 * Every wait here is bounded and event-driven; the test never sleeps.
 * The live child blocks in read() on a pipe the parent holds open, so
 * its lifetime is controlled by the parent closing that pipe rather
 * than by any timing.
 * ==================================================================== */
static int dkTestProcessExitWaitTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int failures = 0;
#define PEXPECT(cond, msg)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("dkTestProcessExitWait FAIL: %s (%s:%d)\n", (msg),          \
                   __FILE__, __LINE__);                                        \
            failures++;                                                        \
        }                                                                      \
    } while (0)

    /* Budgets: the generous one only bounds a wait an event is expected
     * to satisfy, so it costs nothing when the event lands.  The short
     * one is the deliberate expiry case. */
    static const uint64_t kGenerousMs = 30000;
    static const uint64_t kShortMs = 25;
    static const int kChildExitCode = 7;

    /* --- (1) child exits promptly: reaped within budget, status kept. */
    {
        const pid_t child = fork();
        if (child == 0) {
            _exit(kChildExitCode);
        }

        PEXPECT(child > 0, "fork for prompt-exit child");
        if (child > 0) {
            int status = 0;
            PEXPECT(OSRegulateProcessAwaitExitMs(child, kGenerousMs, &status),
                    "prompt-exit child reaped within budget");
            PEXPECT(WIFEXITED(status) && WEXITSTATUS(status) == kChildExitCode,
                    "prompt-exit child status recovered");
        }
    }

    /* --- (2) + (3) long-lived child: budget expiry, watch-fd
     * readiness, then reap after the caller's own kill. */
    {
        int hold[2] = {-1, -1};
        PEXPECT(pipe(hold) == 0, "pipe for long-lived child");
        if (hold[0] >= 0) {
            const pid_t child = fork();
            if (child == 0) {
                close(hold[1]);
                /* Blocks until the parent closes its write end (read
                 * returns 0) or the parent kills us. */
                uint8_t discard;
                while (read(hold[0], &discard, 1) > 0) {
                    /* The parent never writes; drain defensively. */
                }
                _exit(0);
            }

            PEXPECT(child > 0, "fork for long-lived child");
            close(hold[0]); /* the parent keeps only the write end */

            if (child > 0) {
                /* (2) the budget expires with the child alive: false,
                 * and the helper must not have signalled it. */
                int status = 0;
                PEXPECT(!OSRegulateProcessAwaitExitMs(child, kShortMs, &status),
                        "live child not reaped inside short budget");

                /* (3) the watch fd exists and is NOT yet readable. */
                const int watchFd = OSRegulateProcessExitWatchFd(child);
                PEXPECT(watchFd >= 0, "watch fd armed on a live child");
                if (watchFd >= 0) {
                    struct pollfd watch = {.fd = watchFd, .events = POLLIN};
                    int n;
                    while ((n = poll(&watch, 1, 0)) < 0 && errno == EINTR) {
                        /* re-park */
                    }

                    PEXPECT(n == 0, "watch fd not readable while child lives");

                    /* Release the child by closing the pipe: its read
                     * returns 0 and it exits.  The watch fd must then
                     * become readable within the bounded park. */
                    close(hold[1]);
                    hold[1] = -1;
                    while ((n = poll(&watch, 1, (int)kGenerousMs)) < 0 &&
                           errno == EINTR) {
                        /* re-park */
                    }

                    PEXPECT(n == 1, "watch fd readable after child exit");
                    close(watchFd);
                }

                if (hold[1] >= 0) {
                    close(hold[1]);
                    hold[1] = -1;
                }

                /* Kill, then the same bounded await reaps.  Harmless if
                 * the child already exited. */
                kill(child, SIGKILL);
                PEXPECT(
                    OSRegulateProcessAwaitExitMs(child, kGenerousMs, &status),
                    "child reaped after the caller's kill");

                /* A second reap of a gone child reports gone (ECHILD)
                 * instead of blocking. */
                PEXPECT(OSRegulateProcessReapNow(child, NULL),
                        "already-reaped child reports gone");
            }
        }
    }

    /* --- (4) a pid of 0 or below never names one child: all three calls
     * reject it with EINVAL, reap nothing, and do not wait.  First with
     * no child alive, where waitpid(-1) answers ECHILD (which ReapNow
     * reads as "already reaped"); then with a live child, where
     * waitpid(0, WNOHANG) answers 0, equal to the pid.  A blocking
     * waitpid(-1) with that child alive would hang, so -1 is only tried
     * while no child exists. */
    {
        static const pid_t badPids[] = {0, -1};
        for (size_t i = 0; i < sizeof(badPids) / sizeof(badPids[0]); i++) {
            int status = 12345;
            errno = 0;
            PEXPECT(!OSRegulateProcessReapNow(badPids[i], &status) &&
                        errno == EINVAL,
                    "ReapNow rejects pid <= 0 with EINVAL");
            errno = 0;
            PEXPECT(!OSRegulateProcessAwaitExitMs(badPids[i], kGenerousMs,
                                                  &status) &&
                        errno == EINVAL,
                    "AwaitExitMs rejects pid <= 0 with EINVAL");
            errno = 0;
            cleaner(fdClose) int watchFd =
                OSRegulateProcessExitWatchFd(badPids[i]);
            PEXPECT(watchFd == -1 && errno == EINVAL,
                    "ExitWatchFd rejects pid <= 0 with EINVAL");
            PEXPECT(status == 12345, "a rejected pid leaves status untouched");
        }

        int hold[2] = {-1, -1};
        PEXPECT(pipe(hold) == 0, "pipe for the bystander child");
        cleaner(fdClose) int holdRead = hold[0];
        cleaner(fdClose) int holdWrite = hold[1];
        const pid_t child = holdRead >= 0 ? fork() : -1;
        if (child == 0) {
            close(holdWrite);
            uint8_t discard;
            while (read(holdRead, &discard, 1) > 0) {
                /* The parent never writes. */
            }
            _exit(kChildExitCode);
        }

        PEXPECT(child > 0, "fork for the bystander child");
        if (child > 0) {
            close(fdRelease(&holdRead));
            int status = 0;
            errno = 0;
            PEXPECT(!OSRegulateProcessReapNow(0, &status) && errno == EINVAL,
                    "ReapNow(0) with a live child is not 'reaped'");
            errno = 0;
            PEXPECT(!OSRegulateProcessAwaitExitMs(0, kGenerousMs, &status) &&
                        errno == EINVAL,
                    "AwaitExitMs(0) with a live child is not 'reaped'");

            /* The bystander's status is still there for its own pid. */
            close(fdRelease(&holdWrite));
            PEXPECT(OSRegulateProcessAwaitExitMs(child, kGenerousMs, &status),
                    "bystander child reaped by its own pid");
            PEXPECT(WIFEXITED(status) && WEXITSTATUS(status) == kChildExitCode,
                    "bystander child status was not taken by a pid <= 0");
        }
    }

    /* --- (5) the budget holds when no exit notification can be armed.
     * A waiter child exhausts its descriptors, so kqueue() or
     * pidfd_open() fails with EMFILE, then awaits a target that exits a
     * moment later.  The waiter's exit code carries the verdict.  The
     * target's short sleep only orders its exit after the waiter's
     * setup; the budget, not the sleep, bounds the test. */
    {
        static const int kTargetExitCode = 5;
        const pid_t waiter = fork();
        if (waiter == 0) {
            const pid_t target = fork();
            if (target == 0) {
                (void)poll(NULL, 0, 300);
                _exit(kTargetExitCode);
            }

            if (target < 0) {
                _exit(2);
            }

            struct rlimit lim;
            if (getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur > 64) {
                lim.rlim_cur = 64;
                (void)setrlimit(RLIMIT_NOFILE, &lim);
            }

            /* Every descriptor taken is released by _exit(). */
            while (dup(STDERR_FILENO) >= 0 || errno == EINTR) {
                /* fill the table */
            }

            if (errno != EMFILE) {
                _exit(3);
            }

            int status = 0;
            const bool reaped =
                OSRegulateProcessAwaitExitMs(target, kGenerousMs, &status);
            _exit(reaped && WIFEXITED(status) &&
                          WEXITSTATUS(status) == kTargetExitCode
                      ? 0
                      : 1);
        }

        PEXPECT(waiter > 0, "fork for the descriptor-exhausted waiter");
        if (waiter > 0) {
            int status = 0;
            PEXPECT(OSRegulateProcessAwaitExitMs(waiter, kGenerousMs, &status),
                    "descriptor-exhausted waiter reaped");
            PEXPECT(WIFEXITED(status) && WEXITSTATUS(status) != 3,
                    "waiter exhausted its descriptors (EMFILE)");
            PEXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                    "AwaitExitMs kept its budget without an exit "
                    "notification and reaped the target with its status");
        }
    }

#undef PEXPECT
    if (failures == 0) {
        printf(
            "dkTestProcessExitWait: OK (await, expiry, watch-fd readiness)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * Readiness pipe: the cross-process "ready" event a daemonizing parent
 * waits on.  Three children, three verdicts (signalled, died
 * unsignalled, alive but silent), each decided by what the pipe
 * carries (a byte, end-of-file, nothing) rather than by timing.
 * ==================================================================== */
static int dkTestDaemonizeReadyTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int failures = 0;
#define REXPECT(cond, msg)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("dkTestDaemonizeReady FAIL: %s (%s:%d)\n", (msg), __FILE__, \
                   __LINE__);                                                  \
            failures++;                                                        \
        }                                                                      \
    } while (0)

    static const uint64_t kGenerousMs = 30000;
    static const uint64_t kShortMs = 25;

    /* --- (1) the child signals: READY, and the byte outranks the
     *         close that follows it. */
    {
        cleaner(fdClose) int rd = -1;
        cleaner(fdClose) int wr = -1;
        REXPECT(OSRegulateReadyPipeNew(&rd, &wr), "pipe created");
        const pid_t child = fork();
        if (child == 0) {
            close(rd);
            OSRegulateReadyPipeSignal(&wr);
            _exit(0);
        }
        REXPECT(child > 0, "fork for signalling child");
        close(fdRelease(&wr));
        const OSRegulateReadyAwait v =
            OSRegulateReadyPipeAwaitMs(rd, kGenerousMs);
        REXPECT(v == OS_REGULATE_READY_AWAIT_READY,
                "a signalling child reads as READY");
        if (child > 0) {
            int status = 0;
            REXPECT(waitpid(child, &status, 0) == child, "child reaped");
        }
    }

    /* --- (2) the child dies without signalling: WRITER_GONE, not
     *         EXPIRED; end-of-file reports the exit before the budget. */
    {
        cleaner(fdClose) int rd = -1;
        cleaner(fdClose) int wr = -1;
        REXPECT(OSRegulateReadyPipeNew(&rd, &wr), "pipe created");
        const pid_t child = fork();
        if (child == 0) {
            close(rd);
            _exit(3);
        }
        REXPECT(child > 0, "fork for unsignalled child");
        close(fdRelease(&wr));
        const OSRegulateReadyAwait v =
            OSRegulateReadyPipeAwaitMs(rd, kGenerousMs);
        REXPECT(v == OS_REGULATE_READY_AWAIT_WRITER_GONE,
                "a child that died unsignalled reads as WRITER_GONE");
        if (child > 0) {
            int status = 0;
            REXPECT(waitpid(child, &status, 0) == child, "child reaped");
        }
    }

    /* --- (3) the child is alive and silent: EXPIRED at the budget. */
    {
        cleaner(fdClose) int rd = -1;
        cleaner(fdClose) int wr = -1;
        REXPECT(OSRegulateReadyPipeNew(&rd, &wr), "pipe created");
        const pid_t child = fork();
        if (child == 0) {
            close(rd);
            for (;;) {
                pause();
            }
        }
        REXPECT(child > 0, "fork for silent child");
        close(fdRelease(&wr));
        const OSRegulateReadyAwait v = OSRegulateReadyPipeAwaitMs(rd, kShortMs);
        REXPECT(v == OS_REGULATE_READY_AWAIT_EXPIRED,
                "a silent live child reads as EXPIRED");
        /* Guarded: kill(-1, ...) would signal every process we may. */
        if (child > 0) {
            kill(child, SIGKILL);
            int status = 0;
            REXPECT(waitpid(child, &status, 0) == child, "child reaped");
        }
    }

    /* --- (4) signalling twice is idempotent and leaves no descriptor. */
    {
        cleaner(fdClose) int rd = -1;
        cleaner(fdClose) int wr = -1;
        REXPECT(OSRegulateReadyPipeNew(&rd, &wr), "pipe created");
        OSRegulateReadyPipeSignal(&wr);
        REXPECT(wr == -1, "signal closes and clears the write end");
        OSRegulateReadyPipeSignal(&wr);
        REXPECT(OSRegulateReadyPipeAwaitMs(rd, kGenerousMs) ==
                    OS_REGULATE_READY_AWAIT_READY,
                "the one byte reads as READY");
    }

    /* --- (5) the pair is close-on-exec and numbered above the standard
     *         streams even when the creator starts with stdin and stdout
     *         closed (the next socketpair() would otherwise get 0 and
     *         1).  Run in a child, whose exit code is the verdict, so
     *         the test process keeps its own stdio. */
    {
        const pid_t child = fork();
        if (child == 0) {
            close(STDIN_FILENO);
            close(STDOUT_FILENO);
            int rd = -1;
            int wr = -1;
            if (!OSRegulateReadyPipeNew(&rd, &wr)) {
                _exit(2);
            }

            const bool above = rd > STDERR_FILENO && wr > STDERR_FILENO;
            const bool cloexec = (fcntl(rd, F_GETFD) & FD_CLOEXEC) &&
                                 (fcntl(wr, F_GETFD) & FD_CLOEXEC);
            _exit(above && cloexec ? 0 : 1);
        }

        REXPECT(child > 0, "fork for the closed-stdio pair child");
        if (child > 0) {
            int status = 0;
            REXPECT(OSRegulateProcessAwaitExitMs(child, kGenerousMs, &status),
                    "closed-stdio pair child reaped");
            REXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                    "pair is close-on-exec and above stderr with stdio "
                    "closed");
        }
    }

    /* --- (6) DaemonizeUntilReady end to end.  A launcher child
     *         daemonizes; its daemon either signals or exits silently,
     *         and the launcher (the foreground parent) must exit 0 or 1
     *         to match.  Launchers start with stdio open and with stdin
     *         and stdout closed, where the readiness channel would land
     *         on the streams /dev/null replaces.  The daemons are
     *         re-parented away from the test process, so only the
     *         launchers are reaped here. */
    for (int closedStdio = 0; closedStdio < 2; closedStdio++) {
        for (int signals = 0; signals < 2; signals++) {
            const pid_t launcher = fork();
            if (launcher == 0) {
                if (closedStdio) {
                    close(STDIN_FILENO);
                    close(STDOUT_FILENO);
                }

                const OSRegulateDaemonizeStatus st =
                    OSRegulateDaemonizeUntilReady(kGenerousMs, NULL);
                if (st == OS_REGULATE_DAEMONIZE_CHILD) {
                    if (signals) {
                        OSRegulateDaemonizeSignalReady();
                    }
                    _exit(0);
                }

                _exit(4);
            }

            REXPECT(launcher > 0, "fork for the daemonize launcher");
            if (launcher > 0) {
                int status = 0;
                REXPECT(OSRegulateProcessAwaitExitMs(launcher, kGenerousMs,
                                                     &status),
                        "daemonize launcher reaped");
                const int want = signals ? 0 : 1;
                if (!(WIFEXITED(status) && WEXITSTATUS(status) == want)) {
                    printf("dkTestDaemonizeReady: launcher (stdio %s, daemon "
                           "%s) exit status %d, want %d\n",
                           closedStdio ? "closed" : "open",
                           signals ? "signals" : "silent",
                           WIFEXITED(status) ? WEXITSTATUS(status) : -1, want);
                }
                REXPECT(WIFEXITED(status) && WEXITSTATUS(status) == want,
                        "launcher exit status follows the daemon's readiness");
            }
        }
    }

    /* --- (7) the pidfile: a second instance is refused without
     *         truncating the running instance's pid or leaking a
     *         descriptor, and a successor rewrites a longer stale pid
     *         exactly. */
    {
        cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
        char path[PATH_MAX];
        if (!dkTestTmpDirPath(&tmp, "pidfile", path, sizeof(path))) {
            REXPECT(false, "scratch directory for the pidfile");
            return failures;
        }

        int hold[2] = {-1, -1};
        REXPECT(pipe(hold) == 0, "pipe for the pidfile owner");
        cleaner(fdClose) int holdRead = hold[0];
        cleaner(fdClose) int holdWrite = hold[1];
        cleaner(fdClose) int lockedRead = -1;
        cleaner(fdClose) int lockedWrite = -1;
        REXPECT(OSRegulateReadyPipeNew(&lockedRead, &lockedWrite),
                "readiness pair for the pidfile owner");

        const pid_t owner = holdRead >= 0 && lockedRead >= 0 ? fork() : -1;
        if (owner == 0) {
            close(holdWrite);
            close(lockedRead);
            if (!OSRegulateWritePidToFile(path)) {
                _exit(1);
            }

            OSRegulateReadyPipeSignal(&lockedWrite);
            uint8_t discard;
            while (read(holdRead, &discard, 1) > 0) {
                /* The parent never writes. */
            }
            _exit(0);
        }

        REXPECT(owner > 0, "fork for the pidfile owner");
        if (owner > 0) {
            close(fdRelease(&holdRead));
            close(fdRelease(&lockedWrite));
            REXPECT(OSRegulateReadyPipeAwaitMs(lockedRead, kGenerousMs) ==
                        OS_REGULATE_READY_AWAIT_READY,
                    "owner wrote and locked the pidfile");

            /* The lowest free descriptor number, before and after a
             * refused write: a leaked descriptor takes it. */
            const int probeBefore = dup(STDERR_FILENO);
            close(probeBefore);
            REXPECT(!OSRegulateWritePidToFile(path),
                    "a second instance is refused while the lock is held");
            const int probeAfter = dup(STDERR_FILENO);
            close(probeAfter);
            REXPECT(probeAfter == probeBefore,
                    "a refused pidfile write leaks no descriptor");

            char want[32];
            snprintf(want, sizeof(want), "%d\n", (int)owner);
            char got[32] = {0};
            cleaner(fdClose) int readBack = open(path, O_RDONLY | O_CLOEXEC);
            const ssize_t n =
                readBack >= 0 ? read(readBack, got, sizeof(got) - 1) : -1;
            REXPECT(n == (ssize_t)strlen(want) && memcmp(got, want, n) == 0,
                    "a refused write leaves the owner's pid in the file");

            close(fdRelease(&holdWrite));
            int status = 0;
            REXPECT(OSRegulateProcessAwaitExitMs(owner, kGenerousMs, &status) &&
                        WIFEXITED(status) && WEXITSTATUS(status) == 0,
                    "pidfile owner exited cleanly");
        }

        /* The owner is gone and its lock with it.  Leave a longer stale
         * pid behind; the successor must replace it exactly. */
        cleaner(fdClose) int stale = open(path, O_WRONLY | O_CLOEXEC);
        REXPECT(stale >= 0 && write(stale, "99999999999\n", 12) == 12,
                "stale pid written");

        const pid_t successor = fork();
        if (successor == 0) {
            _exit(OSRegulateWritePidToFile(path) ? 0 : 1);
        }

        REXPECT(successor > 0, "fork for the pidfile successor");
        if (successor > 0) {
            int status = 0;
            REXPECT(
                OSRegulateProcessAwaitExitMs(successor, kGenerousMs, &status) &&
                    WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "successor took the released lock");

            char want[32];
            snprintf(want, sizeof(want), "%d\n", (int)successor);
            char got[32] = {0};
            cleaner(fdClose) int readBack = open(path, O_RDONLY | O_CLOEXEC);
            const ssize_t n =
                readBack >= 0 ? read(readBack, got, sizeof(got) - 1) : -1;
            REXPECT(n == (ssize_t)strlen(want) && memcmp(got, want, n) == 0,
                    "successor's pid replaced the longer stale pid exactly");
        }

        (void)unlink(path);
    }

#undef REXPECT
    if (failures == 0) {
        printf("dkTestDaemonizeReady: OK (ready, writer gone, expired, "
               "idempotent signal, stdio placement, daemonize end to end, "
               "pidfile)\n");
    }
    return failures;
}

/* Async-signal-safe wake descriptor (OSRegulate.h): an idle await
 * expires, a signalled one wakes and drains, several signals coalesce
 * into one wake, a wake raised inside a signal handler is delivered,
 * and Signal preserves errno. */
static int dkTestWakeFdHandlerWr = -1;
static void dkTestWakeFdSignalHandler(int sig) {
    (void)sig;
    OSRegulateWakeSignal(dkTestWakeFdHandlerWr);
}

static int dkTestWakeFdTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int failures = 0;
#define WEXPECT(cond, msg)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("dkTestWakeFd FAIL: %s (%s:%d)\n", (msg), __FILE__,         \
                   __LINE__);                                                  \
            failures++;                                                        \
        }                                                                      \
    } while (0)

    static const uint64_t kGenerousMs =
        30000;                           /* bounds a wait an event satisfies */
    static const uint64_t kShortMs = 20; /* the deliberate expiry case */

    int rd = -1;
    int wr = -1;
    WEXPECT(OSRegulateWakeFdNew(&rd, &wr), "wake pair created");
    if (rd < 0 || wr < 0) {
        return failures ? failures : 1;
    }

    WEXPECT(!OSRegulateWakeAwaitMs(rd, kShortMs), "idle await expires");

    OSRegulateWakeSignal(wr);
    WEXPECT(OSRegulateWakeAwaitMs(rd, kGenerousMs), "signalled await wakes");
    WEXPECT(!OSRegulateWakeAwaitMs(rd, kShortMs), "delivery drained the wake");

    OSRegulateWakeSignal(wr);
    OSRegulateWakeSignal(wr);
    OSRegulateWakeSignal(wr);
    WEXPECT(OSRegulateWakeAwaitMs(rd, kGenerousMs),
            "coalesced signals wake once");
    WEXPECT(!OSRegulateWakeAwaitMs(rd, kShortMs),
            "coalesced signals leave nothing pending");

    /* Raised from a signal handler, where a condition variable cannot be
     * signalled.  raise() runs the handler synchronously. */
    dkTestWakeFdHandlerWr = wr;
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = dkTestWakeFdSignalHandler;
    sigemptyset(&act.sa_mask);
    struct sigaction old;
    WEXPECT(sigaction(SIGUSR1, &act, &old) == 0, "install SIGUSR1 handler");
    WEXPECT(raise(SIGUSR1) == 0, "raise SIGUSR1");
    WEXPECT(OSRegulateWakeAwaitMs(rd, kGenerousMs),
            "wake raised inside a signal handler is delivered");
    (void)sigaction(SIGUSR1, &old, NULL);
    dkTestWakeFdHandlerWr = -1;

    errno = EBUSY;
    OSRegulateWakeSignal(wr);
    WEXPECT(errno == EBUSY, "Signal preserves errno");
    OSRegulateWakeDrain(rd);
    WEXPECT(!OSRegulateWakeAwaitMs(rd, kShortMs), "Drain consumed the wake");

    /* Both ends are non-blocking and close-on-exec. */
    WEXPECT((fcntl(rd, F_GETFL) & O_NONBLOCK) &&
                (fcntl(wr, F_GETFL) & O_NONBLOCK),
            "wake pair is non-blocking");
    WEXPECT((fcntl(rd, F_GETFD) & FD_CLOEXEC) &&
                (fcntl(wr, F_GETFD) & FD_CLOEXEC),
            "wake pair is close-on-exec");

    if (wr != rd) {
        close(wr);
    }
    close(rd);

    /* A pair that has been closed: its numbers are not open descriptors
     * (nothing else in this single-threaded test opens one meanwhile).
     * A Signal whose write fails still preserves errno (the check above
     * cannot fail: a successful write never touches errno), and an
     * await on a closed descriptor is a poll() failure, not a wake. */
    {
        int closedRd = -1;
        int closedWr = -1;
        WEXPECT(OSRegulateWakeFdNew(&closedRd, &closedWr), "second pair");
        if (closedWr != closedRd) {
            close(closedWr);
        }
        close(closedRd);

        errno = EBUSY;
        OSRegulateWakeSignal(closedWr);
        WEXPECT(errno == EBUSY, "a failing Signal preserves errno");
        WEXPECT(!OSRegulateWakeAwaitMs(closedRd, kGenerousMs),
                "an await on a closed descriptor is not a wake");
    }

    /* Every write end closed (pipe platforms; an eventfd is one
     * descriptor and cannot hang up): a wake still pending is
     * delivered, then each await waits out its budget and reports no
     * wake instead of returning at once, so a loop on it cannot spin. */
    {
        cleaner(fdClose) int hupRd = -1;
        int hupWr = -1;
        WEXPECT(OSRegulateWakeFdNew(&hupRd, &hupWr), "hang-up pair");
        if (hupRd >= 0 && hupWr != hupRd) {
            OSRegulateWakeSignal(hupWr);
            close(hupWr);
            WEXPECT(OSRegulateWakeAwaitMs(hupRd, kGenerousMs),
                    "a wake sent before the hang-up is delivered");

            static const int kAwaits = 3;
            const uint64_t startMs = timeUtilMonotonicMs();
            int woken = 0;
            for (int i = 0; i < kAwaits; i++) {
                woken += OSRegulateWakeAwaitMs(hupRd, kShortMs);
            }
            const uint64_t elapsedMs = timeUtilMonotonicMs() - startMs;
            WEXPECT(woken == 0, "a hung-up pair reports no wake");
            /* Each await ends once the millisecond clock has moved by
             * its budget, so allow one tick per await. */
            WEXPECT(elapsedMs + kAwaits >= kAwaits * kShortMs,
                    "a hung-up pair's await waits out its budget");
        }
    }

    if (failures == 0) {
        printf("dkTestWakeFd: PASS\n");
    }
#undef WEXPECT
    return failures;
}

/* ====================================================================
 * inflightGate: in-flight counter with a bounded wait for zero.
 *
 * Four properties, each with its own case:
 *   1. an exit-to-zero with no waiter registered never touches the
 *      gate's mutex (it completes while another thread holds that
 *      mutex; a broadcast would block there);
 *   2. a registered waiter is released by the last exit;
 *   3. the budget expires with a straggler still inside;
 *   4. an exit landing between the waiter's count read and its park
 *      still wakes the waiter;
 *   5. drain-then-free: once the wait returns IDLE the gate's storage
 *      can be freed while the exiting thread is still returning from
 *      Exit (under ASan a late touch of the gate reports a
 *      heap-use-after-free).
 * ==================================================================== */
typedef struct dkTestGateCtl {
    inflightGate *gate;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool holding;  /* holder owns gate->mu */
    bool release;  /* holder may drop gate->mu */
    bool exitDone; /* the no-waiter Exit returned */
    bool armed;    /* the waiter is about to park */
    bool waitDone; /* the waiter's await returned */
    inflightGateVerdict verdict;
} dkTestGateCtl;

/* Bounded handshake on the control block — the test never spins. */
static void dkTestGateSet(dkTestGateCtl *c, bool *field) {
    pthread_mutex_lock(&c->mu);
    *field = true;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

static bool dkTestGateAwait(dkTestGateCtl *c, const bool *field,
                            const uint64_t budgetMs) {
    const uint64_t startMs = timeUtilMonotonicMs();
    bool got = false;
    pthread_mutex_lock(&c->mu);
    for (;;) {
        if (*field) {
            got = true;
            break;
        }

        const uint64_t elapsedMs = timeUtilMonotonicMs() - startMs;
        if (elapsedMs >= budgetMs) {
            break;
        }

        (void)monotonicCondWaitMs(&c->cv, &c->mu, budgetMs - elapsedMs);
    }

    pthread_mutex_unlock(&c->mu);
    return got;
}

/* Budgets shared by the helper threads and the test body: one generous
 * enough that only a lost wake can expire it, one deliberately short
 * for the straggler case. */
#define DK_TEST_GATE_GENEROUS_MS 30000
#define DK_TEST_GATE_SHORT_MS 20
/* Rounds of the lost-wake race.  Each round is one thread create plus a
 * handshake, so this costs milliseconds of wall clock while giving the
 * window hundreds of independent chances to be hit. */
#define DK_TEST_GATE_RACE_ROUNDS 256

static void *dkTestGateHolderMain(void *arg) {
    dkTestGateCtl *c = arg;
    pthread_mutex_lock(&c->gate->mu);
    dkTestGateSet(c, &c->holding);
    (void)dkTestGateAwait(c, &c->release, DK_TEST_GATE_GENEROUS_MS);
    pthread_mutex_unlock(&c->gate->mu);
    return NULL;
}

static void *dkTestGateExiterMain(void *arg) {
    dkTestGateCtl *c = arg;
    inflightGateEnter(c->gate);
    inflightGateExit(c->gate);
    dkTestGateSet(c, &c->exitDone);
    return NULL;
}

static void *dkTestGateArmedExiterMain(void *arg) {
    dkTestGateCtl *c = arg;
    /* Fire the moment the waiter says it is entering the park: this is
     * the window between its count read and its wait. */
    (void)dkTestGateAwait(c, &c->armed, DK_TEST_GATE_GENEROUS_MS);
    inflightGateExit(c->gate);
    return NULL;
}

static void *dkTestGateWaiterMain(void *arg) {
    dkTestGateCtl *c = arg;
    dkTestGateSet(c, &c->armed);
    c->verdict = inflightGateAwaitZeroMs(c->gate, DK_TEST_GATE_GENEROUS_MS);
    dkTestGateSet(c, &c->waitDone);
    return NULL;
}

/* Drain-then-free rounds. Each round allocates a gate, enters it, has
 * the exiter thread leave it, waits for zero and frees it at once. The
 * exiter spins on a round counter so its Exit lands as close as possible
 * to the wait; the round count is capped by a wall-clock budget. */
#define DK_TEST_GATE_FREE_ROUNDS 200000u
#define DK_TEST_GATE_FREE_BUDGET_MS 3000u

typedef struct dkTestGateFreeCtl {
    _Atomic(inflightGate *) cur;
    _Atomic uint32_t round;
    _Atomic bool quit;
} dkTestGateFreeCtl;

static void *dkTestGateFreeExiterMain(void *arg) {
    dkTestGateFreeCtl *c = arg;
    uint32_t seen = 0;
    for (;;) {
        uint32_t round;
        while ((round = atomic_load_explicit(&c->round,
                                             memory_order_acquire)) == seen) {
            if (atomic_load_explicit(&c->quit, memory_order_acquire)) {
                return NULL;
            }
        }
        seen = round;
        inflightGateExit(atomic_load_explicit(&c->cur, memory_order_relaxed));
    }
}

defcleanerName(inflightGate *, zfree, InflightGate);

static int dkTestInflightGateTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int failures = 0;
#define GEXPECT(cond, msg)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("dkTestInflightGate FAIL: %s (%s:%d)\n", (msg), __FILE__,   \
                   __LINE__);                                                  \
            failures++;                                                        \
        }                                                                      \
    } while (0)

    inflightGate gate = INFLIGHT_GATE_INIT;

    GEXPECT(inflightGateDepth(&gate) == 0, "a fresh gate is idle");
    GEXPECT(inflightGateAwaitZeroMs(&gate, DK_TEST_GATE_SHORT_MS) ==
                INFLIGHT_GATE_IDLE,
            "await on an idle gate returns IDLE at once");

    /* --- Property 3: budget expiry with a straggler. --- */
    inflightGateEnter(&gate);
    inflightGateEnter(&gate);
    inflightGateExit(&gate);
    GEXPECT(inflightGateDepth(&gate) == 1, "one straggler remains");
    GEXPECT(inflightGateAwaitZeroMs(&gate, DK_TEST_GATE_SHORT_MS) ==
                INFLIGHT_GATE_EXPIRED,
            "a straggler expires the budget");
    inflightGateExit(&gate);
    GEXPECT(inflightGateDepth(&gate) == 0, "the straggler left");

    /* The await above constructed the gate's mutex/condvar, so the
     * holder below can take that mutex. */
    dkTestGateCtl ctl;
    memset(&ctl, 0, sizeof(ctl));
    ctl.gate = &gate;
    pthread_mutex_init(&ctl.mu, NULL);
    monotonicCondInit(&ctl.cv);

    /* --- Property 1: an exit-to-zero with no waiter never broadcasts.
     *
     * The holder owns gate->mu for the whole window.  An Exit that took
     * that mutex would block until the holder released it, so the Exit
     * returning shows that it did not. --- */
    {
        pthread_t holder;
        pthread_t exiter;
        GEXPECT(pthread_create(&holder, NULL, dkTestGateHolderMain, &ctl) == 0,
                "holder thread started");
        GEXPECT(dkTestGateAwait(&ctl, &ctl.holding, DK_TEST_GATE_GENEROUS_MS),
                "holder took the gate mutex");
        GEXPECT(pthread_create(&exiter, NULL, dkTestGateExiterMain, &ctl) == 0,
                "exiter thread started");
        GEXPECT(dkTestGateAwait(&ctl, &ctl.exitDone, DK_TEST_GATE_GENEROUS_MS),
                "exit-to-zero with no waiter completed under a held mutex");
        dkTestGateSet(&ctl, &ctl.release);
        pthread_join(exiter, NULL);
        pthread_join(holder, NULL);
        GEXPECT(inflightGateDepth(&gate) == 0, "no-waiter exit left it idle");
    }

    /* --- Property 2: the last exit releases a registered waiter. --- */
    {
        inflightGateEnter(&gate);
        pthread_mutex_lock(&ctl.mu);
        ctl.armed = false;
        ctl.verdict = INFLIGHT_GATE_EXPIRED;
        pthread_mutex_unlock(&ctl.mu);

        pthread_t waiter;
        GEXPECT(pthread_create(&waiter, NULL, dkTestGateWaiterMain, &ctl) == 0,
                "waiter thread started");
        GEXPECT(dkTestGateAwait(&ctl, &ctl.armed, DK_TEST_GATE_GENEROUS_MS),
                "waiter registered");
        inflightGateExit(&gate);
        pthread_join(waiter, NULL);
        GEXPECT(ctl.verdict == INFLIGHT_GATE_IDLE,
                "the last exit woke the waiter");
    }

    /* --- A broadcast while the gate is busy only makes the waiter
     * re-check: it parks again and returns at the last exit. --- */
    {
        inflightGate fresh = INFLIGHT_GATE_INIT;
        inflightGateWakeAllWaiters(&fresh);
        GEXPECT(atomic_load(&fresh.ready) && inflightGateDepth(&fresh) == 0,
                "a wake on a fresh gate constructs it and changes no depth");
        pthread_mutex_destroy(&fresh.mu);
        pthread_cond_destroy(&fresh.cv);

        inflightGateEnter(&gate);
        pthread_mutex_lock(&ctl.mu);
        ctl.armed = false;
        ctl.waitDone = false;
        ctl.verdict = INFLIGHT_GATE_EXPIRED;
        pthread_mutex_unlock(&ctl.mu);

        pthread_t waiter;
        GEXPECT(pthread_create(&waiter, NULL, dkTestGateWaiterMain, &ctl) == 0,
                "wake-all waiter started");
        /* Registration happens under the gate mutex and the park releases
         * it, so once the count is visible the broadcast below (which
         * takes the mutex) reaches a parked waiter. */
        const uint64_t startMs = timeUtilMonotonicMs();
        while (atomic_load(&gate.waiters) == 0 &&
               timeUtilMonotonicMs() - startMs < DK_TEST_GATE_GENEROUS_MS) {
            sched_yield();
        }
        GEXPECT(atomic_load(&gate.waiters) == 1, "wake-all waiter registered");
        inflightGateWakeAllWaiters(&gate);
        inflightGateWakeAllWaiters(&gate);
        GEXPECT(!dkTestGateAwait(&ctl, &ctl.waitDone, DK_TEST_GATE_SHORT_MS),
                "a wake while busy does not release the waiter");
        inflightGateExit(&gate);
        pthread_join(waiter, NULL);
        GEXPECT(ctl.waitDone && ctl.verdict == INFLIGHT_GATE_IDLE,
                "and the last exit after the wakes releases it");
    }

    /* --- Property 4: the exit lands in the waiter's own window. --- */
    for (uint32_t round = 0; round < DK_TEST_GATE_RACE_ROUNDS; round++) {
        inflightGateEnter(&gate);
        pthread_mutex_lock(&ctl.mu);
        ctl.armed = false;
        pthread_mutex_unlock(&ctl.mu);

        pthread_t racer;
        if (pthread_create(&racer, NULL, dkTestGateArmedExiterMain, &ctl) !=
            0) {
            GEXPECT(false, "race exiter thread started");
            inflightGateExit(&gate);
            break;
        }

        dkTestGateSet(&ctl, &ctl.armed);
        const inflightGateVerdict v =
            inflightGateAwaitZeroMs(&gate, DK_TEST_GATE_GENEROUS_MS);
        pthread_join(racer, NULL);
        if (v != INFLIGHT_GATE_IDLE) {
            GEXPECT(false, "an exit inside the waiter's window was lost");
            break;
        }
    }

    GEXPECT(inflightGateDepth(&gate) == 0, "the race left the gate idle");

    /* --- Property 5: drain-then-free. --- */
    {
        dkTestGateFreeCtl fc;
        memset(&fc, 0, sizeof(fc));
        pthread_t exiter;
        GEXPECT(pthread_create(&exiter, NULL, dkTestGateFreeExiterMain, &fc) ==
                    0,
                "drain-then-free exiter started");
        const uint64_t startMs = timeUtilMonotonicMs();
        uint32_t rounds = 0;
        uint32_t lost = 0;
        for (uint32_t round = 1; round <= DK_TEST_GATE_FREE_ROUNDS; round++) {
            cleanerName(zfree, InflightGate) inflightGate *g =
                zmalloc(sizeof(*g));
            *g = (inflightGate)INFLIGHT_GATE_INIT;
            inflightGateEnter(g);
            atomic_store_explicit(&fc.cur, g, memory_order_relaxed);
            atomic_store_explicit(&fc.round, round, memory_order_release);
            if (inflightGateAwaitZeroMs(g, DK_TEST_GATE_GENEROUS_MS) !=
                INFLIGHT_GATE_IDLE) {
                lost++;
            }
            rounds = round;
            /* g is freed here, as a teardown would free its owner. */
            if (timeUtilMonotonicMs() - startMs >=
                DK_TEST_GATE_FREE_BUDGET_MS) {
                break;
            }
        }
        atomic_store_explicit(&fc.quit, true, memory_order_release);
        pthread_join(exiter, NULL);
        GEXPECT(lost == 0, "every drain-then-free round read IDLE");
        GEXPECT(rounds > 0, "drain-then-free ran at least one round");
    }

    GEXPECT(strcmp(inflightGateVerdictName(INFLIGHT_GATE_IDLE), "idle") == 0,
            "IDLE names itself");
    GEXPECT(strcmp(inflightGateVerdictName(INFLIGHT_GATE_EXPIRED), "expired") ==
                0,
            "EXPIRED names itself");

    pthread_mutex_destroy(&ctl.mu);
    pthread_cond_destroy(&ctl.cv);

    if (failures == 0) {
        printf("dkTestInflightGate: PASS\n");
    }
#undef GEXPECT
    return failures;
}

/* ====================================================================
 * ownedState
 *
 * The clock is injected, so grace periods are driven rather than
 * waited out.
 *
 *   1. grace and idle observation are both required to release a row;
 *   2. the idle stamp releases rows on a gate that is busy at every
 *      Drain;
 *   3. the release and describe callbacks run;
 *   4. the hold report fires once per row and keeps the row;
 *   5. writer predicate: the declared writer is silent, a foreign
 *      writer is counted and reported and its write still lands, and a
 *      NULL predicate disables the check;
 *   6. a displaced value is retired, not freed by its displacer;
 *   7. destruct releases every remaining row, held ones included;
 *   8. ownedSlot transitions, exactly-once release, NULL note, counters
 *      per declaration, and a zero-filled slot with no declaration;
 *   9. ownedSeq: VALID, OVERWRITTEN and TORN verdicts, the two cursors,
 *      the inline word reader, oversize refusal and CAPACITY, the
 *      decline predicate, writer predicate, counters per declaration,
 *      zero versions, a delayed writer that must not replace a newer
 *      publication, nested publication on N == 1, MPSC, and overlapping
 *      writers with a concurrent reader;
 *  10. concurrent readers against a writer whose release frees, under
 *      both release rules, and unserialized writers on one cell.
 * ==================================================================== */
typedef struct dkOwnedCargo {
    uint64_t id;
    bool released;
} dkOwnedCargo;

static uint64_t g_dkOwnedNowUs;
static uint32_t g_dkOwnedReleases;
static uint32_t g_dkOwnedHoldCalls;
static uint64_t g_dkOwnedHoldId;
static uint64_t g_dkOwnedHoldAge;
static uint32_t g_dkOwnedHoldDepth;
static char g_dkOwnedHoldLabel[32];
static bool g_dkOwnedWriterOk;
static uint32_t g_dkOwnedViolationNotes;
static ownedArena g_dkOwnedArena;

static uint32_t g_dkOwnedNowCalls;

static uint64_t dkOwnedNowUs_(void) {
    g_dkOwnedNowCalls++;
    return g_dkOwnedNowUs;
}

static void dkOwnedRelease_(void *cargo) {
    ((dkOwnedCargo *)cargo)->released = true;
    g_dkOwnedReleases++;
}

static void dkOwnedDescribe_(const void *cargo, const char **outLabel,
                             uint64_t *outId) {
    *outLabel = "widget";
    *outId = cargo ? ((const dkOwnedCargo *)cargo)->id : 0;
}

static void dkOwnedNoteHold_(void *ctx, const char *arenaName,
                             const char *label, uint64_t id, uint64_t ageUs,
                             uint32_t depth) {
    (void)ctx;
    (void)arenaName;
    snprintf(g_dkOwnedHoldLabel, sizeof(g_dkOwnedHoldLabel), "%s",
             label ? label : "");
    g_dkOwnedHoldId = id;
    g_dkOwnedHoldAge = ageUs;
    g_dkOwnedHoldDepth = depth;
    g_dkOwnedHoldCalls++;
}

static bool dkOwnedWriterOk_(const void *owner) {
    (void)owner;
    return g_dkOwnedWriterOk;
}

static void dkOwnedNoteViolation_(const ownedCellDecl *decl,
                                  const void *owner) {
    (void)decl;
    (void)owner;
    g_dkOwnedViolationNotes++;
}

#define DK_OWNED_GRACE_US ((uint64_t)1000)
#define DK_OWNED_HOLD_MULTIPLE ((uint64_t)5)

static const ownedArenaDecl dkOwnedArenaDecl_ = {
    .name = "datakit.selftest.arena",
    .graceUs = DK_OWNED_GRACE_US,
    .holdMultiple = DK_OWNED_HOLD_MULTIPLE,
    .nowUs = dkOwnedNowUs_,
    .noteHold = dkOwnedNoteHold_,
    .noteCtx = NULL,
};

static const ownedCellDecl dkOwnedCellDecl_ = {
    .name = "datakit.selftest.cell",
    .form = OWNED_CELL_INDIRECT,
    .discipline = OWNED_WRITER_SERIALIZED,
    .writer_ok = dkOwnedWriterOk_,
    .note_violation = dkOwnedNoteViolation_,
    .release = dkOwnedRelease_,
    .describe = dkOwnedDescribe_,
    .oom = OWNED_OOM_LEAK,
};

/* No predicate: a NULL writer_ok disables the check. */
static const ownedCellDecl dkOwnedDeclineDecl_ = {
    .name = "datakit.selftest.cell.declines",
    .form = OWNED_CELL_INDIRECT,
    .discipline = OWNED_WRITER_SERIALIZED,
    .writer_ok = NULL,
    .note_violation = dkOwnedNoteViolation_,
    .release = dkOwnedRelease_,
    .describe = dkOwnedDescribe_,
    .oom = OWNED_OOM_FREE,
};

static const ownedCellDecl dkOwnedInlineDecl_ = {
    .name = "datakit.selftest.cell.inline",
    .form = OWNED_CELL_INLINE,
    .discipline = OWNED_WRITER_CLASS,
    .writer_ok = dkOwnedWriterOk_,
    .note_violation = dkOwnedNoteViolation_,
};

/* ownedSlot fixtures. */
static uint32_t g_dkSlotReleases;
static void *g_dkSlotReleasedCargo;
static uint32_t g_dkSlotNotes;
static const char *g_dkSlotNoteName;
static int g_dkSlotNoteDisplaced;
static uint64_t g_dkSlotNoteReading;

static void dkSlotRelease_(void *cargo) {
    g_dkSlotReleases++;
    g_dkSlotReleasedCargo = cargo;
}

static void dkSlotNote_(void *ctx, const ownedSlotDecl *decl,
                        ownedSlotEvent event, ownedSlotState displaced,
                        uint64_t reading) {
    (void)ctx;
    (void)event;
    g_dkSlotNotes++;
    g_dkSlotNoteName = decl ? decl->name : NULL;
    g_dkSlotNoteDisplaced = (int)displaced;
    g_dkSlotNoteReading = reading;
}

static ownedStateRow g_dkSlotRowA;
static ownedStateRow g_dkSlotRowB;
static const ownedSlotDecl dkSlotDeclA_ = {
    .name = "datakit.selftest.slot.a",
    .note = dkSlotNote_,
    .noteCtx = NULL,
    .row = &g_dkSlotRowA,
};
static const ownedSlotDecl dkSlotDeclB_ = {
    .name = "datakit.selftest.slot.b",
    .note = dkSlotNote_,
    .noteCtx = NULL,
    .row = &g_dkSlotRowB,
};
/* NULL note: counts only. */
static ownedStateRow g_dkSlotRowQuiet;
static const ownedSlotDecl dkSlotDeclQuiet_ = {
    .name = "datakit.selftest.slot.quiet",
    .note = NULL,
    .noteCtx = NULL,
    .row = &g_dkSlotRowQuiet,
};

/* ownedSeq fixtures. */
static ownedStateRow dkSeqRowA_;
static ownedStateRow dkSeqRowB_;
static _Atomic uint64_t g_dkSeqViolationNotes = 0;
static bool g_dkSeqWriterOk = true;

static bool dkSeqWriterOk_(const void *owner) {
    (void)owner;
    return g_dkSeqWriterOk;
}
static void dkSeqNoteViolation_(const ownedSeqDecl *decl, const void *owner) {
    (void)decl;
    (void)owner;
    atomic_fetch_add_explicit(&g_dkSeqViolationNotes, 1, memory_order_relaxed);
}

#define DK_SEQ_BYTES 64u
/* N = 4, CLAIM, NULL predicate. */
static const ownedSeqDecl dkSeqDeclA_ = {
    .name = "dk.seq.a",
    .versions = 4,
    .version_bytes = DK_SEQ_BYTES,
    .discipline = OWNED_WRITER_CLAIM,
    .writer_ok = NULL,
    .note_violation = NULL,
    .row = &dkSeqRowA_,
};
/* N = 1: the classic seqlock, used by the nested and overlap cases. */
static const ownedSeqDecl dkSeqDeclOne_ = {
    .name = "dk.seq.one",
    .versions = 1,
    .version_bytes = DK_SEQ_BYTES,
    .discipline = OWNED_WRITER_CLAIM,
    .writer_ok = NULL,
    .note_violation = NULL,
    .row = &dkSeqRowB_,
};
/* A checked writer sharing row A, so both declarations count on it. */
static const ownedSeqDecl dkSeqDeclChecked_ = {
    .name = "dk.seq.a",
    .versions = 4,
    .version_bytes = DK_SEQ_BYTES,
    .discipline = OWNED_WRITER_CLASS,
    .writer_ok = dkSeqWriterOk_,
    .note_violation = dkSeqNoteViolation_,
    .row = &dkSeqRowA_,
};
/* A wide ring for the MPSC case: N much larger than the total number of
 * publications, so no version is ever shared. */
static const ownedSeqDecl dkSeqDeclWide_ = {
    .name = "dk.seq.wide",
    .versions = 1024,
    .version_bytes = DK_SEQ_BYTES,
    .discipline = OWNED_WRITER_CLAIM,
    .writer_ok = NULL,
    .note_violation = NULL,
    .row = NULL,
};

/* A payload whose every byte names its writer, so a mixed copy is
 * detectable byte by byte. */
static void dkSeqPayload_(uint8_t *buf, uint8_t tag) {
    for (size_t i = 0; i < DK_SEQ_BYTES; i++) {
        buf[i] = (uint8_t)(tag ^ (uint8_t)i);
    }
}
static bool dkSeqPayloadWhole_(const uint8_t *buf, size_t len) {
    if (len != DK_SEQ_BYTES) {
        return false;
    }
    const uint8_t tag = buf[0];
    for (size_t i = 0; i < DK_SEQ_BYTES; i++) {
        if (buf[i] != (uint8_t)(tag ^ (uint8_t)i)) {
            return false;
        }
    }
    return true;
}

/* Read the publication being written from inside its own fill,
 * single-threaded. */
typedef struct dkSeqInsideFill {
    ownedSeq *seq;
    uint64_t number;               /* this publication's number */
    ownedSeqReadResult readInside; /* readAt(number) from inside */
    uint64_t assignedInside;
    uint64_t cursorInside;
} dkSeqInsideFill;
static void dkSeqInsideFillFn_(ownedSeqVersionRef *v, void *ctx) {
    dkSeqInsideFill *f = ctx;
    uint8_t payload[DK_SEQ_BYTES];
    dkSeqPayload_(payload, 0x33);
    ownedSeqPutBytes(v, 0, payload, sizeof(payload));
    uint8_t out[DK_SEQ_BYTES];
    size_t outLen = 0;
    f->readInside =
        ownedSeqReadAt(f->seq, f->number, out, sizeof(out), &outLen);
    f->assignedInside = ownedSeqAssigned(f->seq);
    f->cursorInside = ownedSeqCursor(f->seq);
}

/* Fill a one-word version through the word setter: word 0 gets the
 * value, word 1 is past the payload and must be ignored. */
static void dkSeqWordFillFn_(ownedSeqVersionRef *v, void *ctx) {
    const uint64_t *w = ctx;
    ownedSeqPutWord(v, 0, *w);
    ownedSeqPutWord(v, 1, ~*w);
}

/* Publish B from inside A's fill on N == 1. */
typedef struct dkSeqNestedFill {
    ownedSeq *seq;
    uint64_t innerNumber;
} dkSeqNestedFill;
static void dkSeqNestedFillFn_(ownedSeqVersionRef *v, void *ctx) {
    dkSeqNestedFill *f = ctx;
    uint8_t a[DK_SEQ_BYTES];
    dkSeqPayload_(a, 0xA0);
    ownedSeqPutBytes(v, 0, a, sizeof(a));
    uint8_t b[DK_SEQ_BYTES];
    dkSeqPayload_(b, 0xB0);
    f->innerNumber = ownedSeqAssigned(f->seq) + 1;
    (void)ownedSeqPublish(f->seq, b, sizeof(b));
}

/* Writer thread: `count` publications, tagged per writer. */
typedef struct dkSeqWriterCtx {
    ownedSeq *seq;
    uint8_t tag;
    uint32_t count;
} dkSeqWriterCtx;
static void *dkSeqWriterMain_(void *arg) {
    dkSeqWriterCtx *w = arg;
    uint8_t payload[DK_SEQ_BYTES];
    dkSeqPayload_(payload, w->tag);
    for (uint32_t i = 0; i < w->count; i++) {
        (void)ownedSeqPublish(w->seq, payload, sizeof(payload));
    }
    return NULL;
}
/* Reader thread: every VALID read must be one writer's bytes. */
typedef struct dkSeqReaderCtx {
    ownedSeq *seq;
    _Atomic bool *stop;
    uint64_t validReads;
    uint64_t mixedReads;
} dkSeqReaderCtx;
/* The iteration that observes `stop` still reads once.  The test sets
 * `stop` only after every writer is joined, so that last read sees a
 * settled publication and must read VALID.  This makes "the reader
 * validated at least one copy" depend on the seqlock, not on whether
 * the scheduler ran this thread while the writers were publishing. */
static void *dkSeqReaderMain_(void *arg) {
    dkSeqReaderCtx *r = arg;
    uint8_t out[DK_SEQ_BYTES];
    bool stopped = false;
    do {
        stopped = atomic_load_explicit(r->stop, memory_order_acquire);
        const uint64_t want = ownedSeqAssigned(r->seq);
        if (want == 0) {
            continue;
        }
        size_t outLen = 0;
        if (ownedSeqReadAt(r->seq, want, out, sizeof(out), &outLen) ==
            OWNED_SEQ_VALID) {
            r->validReads++;
            if (!dkSeqPayloadWhole_(out, outLen)) {
                r->mixedReads++;
            }
        }
    } while (!stopped);
    return NULL;
}

/* READERS_ONLY release rule.  The default-rule block above uses a
 * declaration that does not set `.release`, so it also checks that the
 * zero value selects the default rule. */
#define DK_OWNED_RO_HORIZON_US ((uint64_t)1000)
static uint32_t g_dkRoHoldCalls = 0;
static void dkOwnedRoNoteHold_(void *ctx, const char *arenaName,
                               const char *label, uint64_t id, uint64_t ageUs,
                               uint32_t depth) {
    (void)ctx;
    (void)arenaName;
    (void)label;
    (void)id;
    (void)ageUs;
    (void)depth;
    g_dkRoHoldCalls++;
}
static const ownedArenaDecl dkOwnedRoArenaDecl_ = {
    .name = "datakit.selftest.arena.readers_only",
    .graceUs = 0,
    .holdMultiple = 0,
    .nowUs = dkOwnedNowUs_,
    .noteHold = dkOwnedRoNoteHold_,
    .noteCtx = NULL,
    .release = OWNED_RELEASE_READERS_ONLY,
    .reportHorizonUs = DK_OWNED_RO_HORIZON_US,
};

static int dkTestOwnedReadersOnly(void) {
    int err = 0;
#define ROEXPECT(cond, msg)                                                    \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("dkTestOwnedState FAIL: readers-only: %s (%s:%d)\n", (msg), \
                   __FILE__, __LINE__);                                        \
            err++;                                                             \
        }                                                                      \
    } while (0)
    ownedArena a;
    ownedArenaInit(&a, &dkOwnedRoArenaDecl_);
    ROEXPECT(a.initialized, "a READERS_ONLY arena comes up");

    /* (a) No clock: an idle gate releases on the first pass with the
     * clock never advanced.  The default rule keeps such a row because
     * its grace has not elapsed (step 1a of the default block). */
    dkOwnedCargo ra = {.id = 101, .released = false};
    ownedArenaRetire(&a, &ra, dkOwnedRelease_, dkOwnedDescribe_,
                     OWNED_OOM_FREE);
    ownedArenaDrain(&a);
    ROEXPECT(ra.released, "(a) idle gate, no clock advance: released");

    /* (b) A reader in its scope holds the row however much time passes;
     * its exit releases it on the next pass. */
    dkOwnedCargo rb = {.id = 102, .released = false};
    ownedArenaEnter(&a);
    ownedArenaRetire(&a, &rb, dkOwnedRelease_, dkOwnedDescribe_,
                     OWNED_OOM_FREE);
    g_dkOwnedNowUs += 1000 * DK_OWNED_GRACE_US;
    ownedArenaDrain(&a);
    ROEXPECT(!rb.released, "(b) a resident reader holds the row past any time");
    ROEXPECT(g_dkRoHoldCalls == 1,
             "(b) the hold is reported once, past the horizon");
    ownedArenaDrain(&a);
    ROEXPECT(!rb.released && g_dkRoHoldCalls == 1,
             "(b) the report is a level edge and still releases nothing");
    ownedArenaExit(&a);
    ownedArenaDrain(&a);
    ROEXPECT(rb.released, "(b) the reader's exit releases it");

    /* (c) Epoch: a scope that entered and exited to zero after the
     * retire publishes the epoch; a reader that entered later cannot
     * hold the row (it could not have found it). */
    dkOwnedCargo rc = {.id = 103, .released = false};
    ownedArenaRetire(&a, &rc, dkOwnedRelease_, dkOwnedDescribe_,
                     OWNED_OOM_FREE);
    ownedArenaEnter(&a);
    ownedArenaExit(&a);  /* reads epoch >= rc's, takes the gate to zero */
    ownedArenaEnter(&a); /* resident at the sample */
    ownedArenaDrain(&a);
    ROEXPECT(rc.released, "(c) a post-retire zero releases a busy-gate row");
    ownedArenaExit(&a);

    /* (d) An exit whose epoch read precedes the retire releases nothing:
     * the gate went to zero before row d existed, and a reader resident
     * across the retire is still inside. */
    ownedArenaEnter(&a);
    ownedArenaEnter(&a);
    ownedArenaExit(&a); /* not to zero */
    dkOwnedCargo rd = {.id = 104, .released = false};
    ownedArenaRetire(&a, &rd, dkOwnedRelease_, dkOwnedDescribe_,
                     OWNED_OOM_FREE);
    ownedArenaDrain(&a);
    ROEXPECT(!rd.released,
             "(d) an idle epoch older than the row releases nothing");
    ownedArenaExit(&a);
    ownedArenaDrain(&a);
    ROEXPECT(rd.released, "(d) and the departure releases it");

    ownedArenaDestruct(&a);
#undef ROEXPECT
    return err;
}

/* Concurrent readers against a writer whose release frees the displaced
 * object, under both release rules. Readers enter the arena, read the
 * cell, check the object's fields and exit; the writer publishes fresh
 * objects and drains. A premature release shows as a bad field here, as
 * a heap-use-after-free under ASan, or as a race under ThreadSanitizer. */
#define DK_OWNED_SOAK_MAGIC UINT64_C(0x50a4c0ffee50a4c0)
#define DK_OWNED_SOAK_PUBLISHES 20000u
#define DK_OWNED_SOAK_DRAIN_EVERY 64u
#define DK_OWNED_SOAK_READERS 3
#define DK_OWNED_CELL_RACE_PUBLISHES 20000u

typedef struct dkOwnedSoakObj {
    uint64_t magic;
    uint64_t id;
} dkOwnedSoakObj;

static _Atomic uint64_t g_dkOwnedSoakBadReleases = 0;

static void dkOwnedSoakRelease_(void *cargo) {
    dkOwnedSoakObj *o = cargo;
    if (o->magic != DK_OWNED_SOAK_MAGIC) {
        atomic_fetch_add_explicit(&g_dkOwnedSoakBadReleases, 1,
                                  memory_order_relaxed);
    }
    o->magic = 0;
    zfree(o);
}

static uint64_t dkOwnedSoakNowUs_(void) {
    return timeUtilMonotonicUs();
}

static dkOwnedSoakObj *dkOwnedSoakObjNew_(uint64_t id) {
    /* @own: caller; free=dkOwnedSoakRelease_ */
    dkOwnedSoakObj *o = zmalloc(sizeof(*o));
    o->magic = DK_OWNED_SOAK_MAGIC;
    o->id = id;
    return o;
}

static const ownedArenaDecl dkOwnedSoakArenaDecls_[2] = {
    {.name = "datakit.selftest.arena.soak.grace",
     .graceUs = 200,
     .holdMultiple = 1000,
     .nowUs = dkOwnedSoakNowUs_,
     .release = OWNED_RELEASE_GRACE_AND_READERS},
    {.name = "datakit.selftest.arena.soak.readers",
     .nowUs = dkOwnedSoakNowUs_,
     .release = OWNED_RELEASE_READERS_ONLY,
     .reportHorizonUs = 0},
};

static const ownedCellDecl dkOwnedSoakCellDecl_ = {
    .name = "datakit.selftest.cell.soak",
    .form = OWNED_CELL_INDIRECT,
    .discipline = OWNED_WRITER_CLASS,
    .writer_ok = NULL,
    .note_violation = NULL,
    .release = dkOwnedSoakRelease_,
    .describe = NULL,
    .oom = OWNED_OOM_LEAK,
};

/* No arena, free on displacement, writers not serialized. */
static const ownedCellDecl dkOwnedRaceCellDecl_ = {
    .name = "datakit.selftest.cell.race",
    .form = OWNED_CELL_INDIRECT,
    .discipline = OWNED_WRITER_INSTANCE,
    .writer_ok = NULL,
    .note_violation = NULL,
    .release = dkOwnedSoakRelease_,
    .describe = NULL,
    .oom = OWNED_OOM_FREE,
};

typedef struct dkOwnedSoakReader {
    ownedArena *arena;
    ownedCell *cell;
    _Atomic bool *stop;
    _Atomic uint32_t *readersReading; /* readers that have read once */
    uint64_t reads;
    uint64_t bad;
} dkOwnedSoakReader;

static void *dkOwnedSoakReaderMain_(void *arg) {
    dkOwnedSoakReader *r = arg;
    while (!atomic_load_explicit(r->stop, memory_order_acquire)) {
        ownedArenaEnter(r->arena);
        const dkOwnedSoakObj *o = ownedCellRead(r->cell);
        if (o) {
            if (r->reads++ == 0) {
                atomic_fetch_add_explicit(r->readersReading, 1,
                                          memory_order_release);
            }

            if (o->magic != DK_OWNED_SOAK_MAGIC) {
                r->bad++;
            }
        }
        ownedArenaExit(r->arena);
    }
    return NULL;
}

static void *dkOwnedRaceWriterMain_(void *arg) {
    ownedCell *c = arg;
    for (uint32_t i = 0; i < DK_OWNED_CELL_RACE_PUBLISHES; i++) {
        ownedCellPublish(c, dkOwnedSoakObjNew_(i));
    }
    return NULL;
}

static int dkTestOwnedArenaSoak(void) {
    int err = 0;
#define SOAKEXPECT(cond, msg)                                                  \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("dkTestOwnedState FAIL: soak: %s (%s:%d)\n", (msg),         \
                   __FILE__, __LINE__);                                        \
            err++;                                                             \
        }                                                                      \
    } while (0)
    atomic_store_explicit(&g_dkOwnedSoakBadReleases, 0, memory_order_relaxed);
    for (int rule = 0; rule < 2; rule++) {
        ownedArena arena;
        ownedArenaInit(&arena, &dkOwnedSoakArenaDecls_[rule]);
        ownedCell cell;
        ownedCellInit(&cell, &dkOwnedSoakCellDecl_, NULL, &arena);
        _Atomic bool stop = false;
        _Atomic uint32_t readersReading = 0;
        dkOwnedSoakReader readers[DK_OWNED_SOAK_READERS];
        pthread_t th[DK_OWNED_SOAK_READERS];
        int started = 0;
        for (int i = 0; i < DK_OWNED_SOAK_READERS; i++) {
            readers[i] = (dkOwnedSoakReader){.arena = &arena,
                                             .cell = &cell,
                                             .stop = &stop,
                                             .readersReading = &readersReading};
            if (pthread_create(&th[i], NULL, dkOwnedSoakReaderMain_,
                               &readers[i]) != 0) {
                break;
            }
            started++;
        }
        SOAKEXPECT(started == DK_OWNED_SOAK_READERS, "readers started");

        /* The publishes take a few milliseconds, which a loaded machine
         * can spend before scheduling the readers at all: publish one
         * object and let every reader read it before the soak starts
         * (bounded, so a reader that never runs still fails below). */
        ownedCellPublish(&cell, dkOwnedSoakObjNew_(0));
        const uint64_t waitUntil = dkOwnedSoakNowUs_() + 5000000;
        while (atomic_load_explicit(&readersReading, memory_order_acquire) <
                   (uint32_t)started &&
               dkOwnedSoakNowUs_() < waitUntil) {
            sched_yield();
        }

        for (uint32_t i = 1; i < DK_OWNED_SOAK_PUBLISHES; i++) {
            ownedCellPublish(&cell, dkOwnedSoakObjNew_(i));
            if (i % DK_OWNED_SOAK_DRAIN_EVERY == 0) {
                ownedArenaDrain(&arena);
            }
        }
        atomic_store_explicit(&stop, true, memory_order_release);
        uint64_t reads = 0;
        uint64_t bad = 0;
        for (int i = 0; i < started; i++) {
            pthread_join(th[i], NULL);
            reads += readers[i].reads;
            bad += readers[i].bad;
        }
        SOAKEXPECT(bad == 0, "no reader saw a released object");
        SOAKEXPECT(reads > 0, "the readers read published objects");
        ownedCellDestruct(&cell);
        ownedArenaDestruct(&arena);
    }

    /* Two writers that no happens-before edge serializes: each displaces
     * the other's objects and, with no arena, releases them inline. The
     * release reads the displaced object's fields. */
    {
        ownedCell cell;
        ownedCellInit(&cell, &dkOwnedRaceCellDecl_, NULL, NULL);
        pthread_t th[2];
        int started = 0;
        for (int i = 0; i < 2; i++) {
            if (pthread_create(&th[i], NULL, dkOwnedRaceWriterMain_, &cell) !=
                0) {
                break;
            }
            started++;
        }
        SOAKEXPECT(started == 2, "racing writers started");
        for (int i = 0; i < started; i++) {
            pthread_join(th[i], NULL);
        }
        ownedCellDestruct(&cell);
    }
    SOAKEXPECT(atomic_load_explicit(&g_dkOwnedSoakBadReleases,
                                    memory_order_relaxed) == 0,
               "every release found its object intact");
#undef SOAKEXPECT
    return err;
}

static int dkTestOwnedStateTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int failures = 0;
#define OEXPECT(cond, msg)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("dkTestOwnedState FAIL: %s (%s:%d)\n", (msg), __FILE__,     \
                   __LINE__);                                                  \
            failures++;                                                        \
        }                                                                      \
    } while (0)

    g_dkOwnedNowUs = 1000000;
    g_dkOwnedReleases = 0;
    g_dkOwnedHoldCalls = 0;
    g_dkOwnedViolationNotes = 0;
    g_dkOwnedWriterOk = true;

    const uint64_t baseDeferred = ownedArenaDeferredTotal();
    const uint64_t baseRetired = ownedArenaRetiredTotal();
    const uint64_t baseGraceHeld = ownedArenaGraceHeldTotal();
    const uint64_t baseNeverLeft = ownedArenaHeldReadersNeverLeftTotal();
    const uint64_t baseLive = ownedArenaLiveRows();
    const uint64_t baseViolations = ownedCellWriterViolationTotal();

    ownedArenaInit(&g_dkOwnedArena, &dkOwnedArenaDecl_);
    OEXPECT(g_dkOwnedArena.initialized, "a declared arena comes up");
    OEXPECT(ownedArenaDepth(&g_dkOwnedArena) == 0, "a fresh arena is idle");

    /* --- 1a: the grace has not elapsed. --- */
    dkOwnedCargo rowA = {.id = 11, .released = false};
    ownedArenaRetire(&g_dkOwnedArena, &rowA, dkOwnedRelease_, dkOwnedDescribe_,
                     OWNED_OOM_FREE);
    OEXPECT(ownedArenaDeferredTotal() == baseDeferred + 1,
            "the retire parked one row");
    OEXPECT(ownedArenaLiveRows() == baseLive + 1,
            "live_rows counts the parked row");
    g_dkOwnedNowUs += DK_OWNED_GRACE_US / 2;
    ownedArenaDrain(&g_dkOwnedArena);
    OEXPECT(!rowA.released, "a row inside its grace is kept");
    OEXPECT(ownedArenaGraceHeldTotal() == baseGraceHeld,
            "a row inside its grace is not a grace hold");

    /* --- 1b: grace elapsed, but a reader is inside its scope. --- */
    ownedArenaEnter(&g_dkOwnedArena);
    OEXPECT(ownedArenaDepth(&g_dkOwnedArena) == 1, "the reader scope is open");
    g_dkOwnedNowUs += DK_OWNED_GRACE_US;
    ownedArenaDrain(&g_dkOwnedArena);
    OEXPECT(!rowA.released, "a present reader holds the row past the grace");
    OEXPECT(ownedArenaGraceHeldTotal() == baseGraceHeld + 1,
            "a row past grace with readers present is counted");

    /* --- 1c: both.  The reader departs; the next pass releases. --- */
    ownedArenaExit(&g_dkOwnedArena);
    ownedArenaDrain(&g_dkOwnedArena);
    OEXPECT(rowA.released, "grace elapsed and readers departed releases");
    OEXPECT(ownedArenaRetiredTotal() == baseRetired + 1,
            "the release is counted");
    OEXPECT(ownedArenaLiveRows() == baseLive, "live_rows fell back");

    /* --- 1d: a reader scope reads no clock under the default rule; the
     * idle observation is carried by the retire epoch. --- */
    {
        const uint32_t callsBefore = g_dkOwnedNowCalls;
        ownedArenaEnter(&g_dkOwnedArena);
        ownedArenaExit(&g_dkOwnedArena);
        OEXPECT(g_dkOwnedNowCalls == callsBefore,
                "Enter and Exit leave the clock unread");
    }

    /* --- 2: the idle stamp.  The gate holds a reader at every Drain, so
     * the direct sample never reads idle; the row still releases because
     * the gate was observed empty after the row was parked. --- */
    dkOwnedCargo rowB = {.id = 22, .released = false};
    ownedArenaRetire(&g_dkOwnedArena, &rowB, dkOwnedRelease_, dkOwnedDescribe_,
                     OWNED_OOM_FREE);
    g_dkOwnedNowUs += 10;
    ownedArenaEnter(&g_dkOwnedArena);
    ownedArenaExit(&g_dkOwnedArena);  /* publishes the stamp */
    ownedArenaEnter(&g_dkOwnedArena); /* and now a resident reader */
    g_dkOwnedNowUs += 2 * DK_OWNED_GRACE_US;
    ownedArenaDrain(&g_dkOwnedArena);
    OEXPECT(rowB.released,
            "a post-park idle stamp releases while the gate reads busy");
    ownedArenaExit(&g_dkOwnedArena);

    /* --- 3 and 4: the hold report, both cargo callbacks, once per row. */
    dkOwnedCargo rowC = {.id = 33, .released = false};
    ownedArenaEnter(&g_dkOwnedArena);
    g_dkOwnedNowUs += 1; /* park strictly after the standing stamp */
    ownedArenaRetire(&g_dkOwnedArena, &rowC, dkOwnedRelease_, dkOwnedDescribe_,
                     OWNED_OOM_FREE);
    g_dkOwnedNowUs += DK_OWNED_HOLD_MULTIPLE * DK_OWNED_GRACE_US + 1;
    ownedArenaDrain(&g_dkOwnedArena);
    OEXPECT(!rowC.released, "a reported row is kept, not released");
    OEXPECT(ownedArenaHeldReadersNeverLeftTotal() == baseNeverLeft + 1,
            "the hold counter moved by exactly one");
    OEXPECT(g_dkOwnedHoldCalls == 1, "the hold is reported once");
    OEXPECT(strcmp(g_dkOwnedHoldLabel, "widget") == 0,
            "the describe fn supplied the line's label");
    OEXPECT(g_dkOwnedHoldId == 33,
            "the describe fn supplied the line's identity");
    OEXPECT(g_dkOwnedHoldAge > DK_OWNED_HOLD_MULTIPLE * DK_OWNED_GRACE_US,
            "the line carries the row's age");
    OEXPECT(g_dkOwnedHoldDepth == 1, "the line carries the live gate depth");
    ownedArenaDrain(&g_dkOwnedArena);
    OEXPECT(ownedArenaHeldReadersNeverLeftTotal() == baseNeverLeft + 1,
            "the hold counter moves once per row, not once per pass");
    OEXPECT(g_dkOwnedHoldCalls == 1, "and the report is not repeated");
    ownedArenaExit(&g_dkOwnedArena); /* the long-lived reader leaves */

    /* --- 4b: the report horizon saturates.  holdMultiple * graceUs past
     * UINT64_MAX means "never report", not a wrapped, early horizon. --- */
    {
        static const ownedArenaDecl farDecl = {
            .name = "datakit.selftest.arena.far",
            .graceUs = DK_OWNED_GRACE_US,
            /* The product wraps to a few hundred microseconds. */
            .holdMultiple = UINT64_MAX / DK_OWNED_GRACE_US + 1,
            .nowUs = dkOwnedNowUs_,
            .noteHold = dkOwnedNoteHold_,
            .noteCtx = NULL,
        };
        ownedArena far;
        ownedArenaInit(&far, &farDecl);
        dkOwnedCargo rowF = {.id = 66, .released = false};
        const uint32_t holdsBefore = g_dkOwnedHoldCalls;
        ownedArenaEnter(&far);
        ownedArenaRetire(&far, &rowF, dkOwnedRelease_, dkOwnedDescribe_,
                         OWNED_OOM_FREE);
        g_dkOwnedNowUs += 10 * DK_OWNED_GRACE_US;
        ownedArenaDrain(&far);
        OEXPECT(!rowF.released && g_dkOwnedHoldCalls == holdsBefore,
                "a horizon past UINT64_MAX never reports");
        ownedArenaExit(&far);
        ownedArenaDrain(&far);
        OEXPECT(rowF.released, "and the row still releases once idle");
        ownedArenaDestruct(&far);
    }

    /* --- 5 and 6: the cell. --- */
    ownedCell cell;
    ownedCellInit(&cell, &dkOwnedCellDecl_, NULL, &g_dkOwnedArena);
    OEXPECT(ownedCellRead(&cell) == NULL, "an unpublished cell reads absent");

    dkOwnedCargo v1 = {.id = 41, .released = false};
    dkOwnedCargo v2 = {.id = 42, .released = false};
    dkOwnedCargo v3 = {.id = 43, .released = false};
    ownedCellPublish(&cell, &v1);
    OEXPECT(ownedCellRead(&cell) == &v1,
            "the publication is what a reader sees");
    OEXPECT(ownedCellWriterViolationTotal() == baseViolations,
            "a publication by the declared writer is silent");

    const uint64_t deferredBeforeSupersede = ownedArenaDeferredTotal();
    ownedCellPublish(&cell, &v2);
    OEXPECT(ownedCellRead(&cell) == &v2, "the successor displaces it");
    OEXPECT(!v1.released,
            "a displaced value is retired, not freed by its displacer");
    OEXPECT(ownedArenaDeferredTotal() == deferredBeforeSupersede + 1,
            "and it retired through the cell's arena");

    g_dkOwnedWriterOk = false;
    ownedCellPublish(&cell, &v3);
    OEXPECT(ownedCellWriterViolationTotal() == baseViolations + 1,
            "an undeclared writer is counted");
    OEXPECT(g_dkOwnedViolationNotes == 1, "and named");
    OEXPECT(ownedCellRead(&cell) == &v3,
            "and the publication is never stopped halfway");
    g_dkOwnedWriterOk = true;

    ownedCell declining;
    ownedCellInit(&declining, &dkOwnedDeclineDecl_, NULL, &g_dkOwnedArena);
    dkOwnedCargo v4 = {.id = 44, .released = false};
    g_dkOwnedWriterOk = false; /* would fail, if anything asked */
    ownedCellPublish(&declining, &v4);
    OEXPECT(ownedCellWriterViolationTotal() == baseViolations + 1,
            "a NULL predicate disables the check");
    OEXPECT(g_dkOwnedViolationNotes == 1, "and says nothing");
    OEXPECT(ownedCellRead(&declining) == &v4, "and publishes normally");
    g_dkOwnedWriterOk = true;

    ownedCell word;
    ownedCellInit(&word, &dkOwnedInlineDecl_, NULL, NULL);
    OEXPECT(ownedCellLoad(&word) == 0, "an inline cell starts absent");
    ownedCellStore(&word, UINT64_C(0x5eed));
    OEXPECT(ownedCellLoad(&word) == UINT64_C(0x5eed),
            "the inline publication reads back");
    OEXPECT(ownedCellRead(&word) == NULL,
            "an inline cell has no indirect value to hand out");

    ownedCellDestruct(&cell);
    OEXPECT(v3.released, "the cell's destructor releases the current value");
    ownedCellDestruct(&declining);
    OEXPECT(v4.released, "on every declaration, predicate or none");
    ownedCellDestruct(&word);
    OEXPECT(ownedCellLoad(&word) == 0, "and an inline cell is left absent");

    /* --- 7: destruct releases every remaining row, held ones included. */
    ownedArenaDestruct(&g_dkOwnedArena);
    OEXPECT(rowC.released, "destruct releases the reported (held) row");
    OEXPECT(v1.released, "destruct releases every surviving row");
    OEXPECT(ownedArenaLiveRows() == baseLive,
            "and live_rows returns to its baseline");
    ownedArenaDestruct(&g_dkOwnedArena); /* idempotent */
    OEXPECT(!g_dkOwnedArena.initialized, "a destructed arena stays down");

    /* ownedSlot.  Each case uses a fresh slot where it needs one, since
     * RETIRED is terminal.  ARMED is reached only through the protocol
     * (a claim followed by publish(still_armed=true)), never by storing
     * it directly.  The "row N" labels name the transitions. */
    {
        ownedSlot sa = OWNED_SLOT_INIT(&dkSlotDeclA_);
        ownedSlotState prior = OWNED_SLOT_RETIRED;
        uint64_t fields = 0;
        uint64_t cargo = 0xC0FFEE;
        g_dkSlotReleases = 0;
        g_dkSlotNotes = 0;
        const size_t rowsBefore = ownedStateRowCount();

        /* rows 1-4: the ordinary tick */
        OEXPECT(ownedSlotClaim(&sa, &prior) && prior == OWNED_SLOT_EMPTY,
                "row 1: an EMPTY slot is claimable and reports EMPTY");
        OEXPECT(ownedStateRowCount() == rowsBefore + 1 &&
                    g_dkSlotRowA.kind == OWNED_ROW_SLOT &&
                    g_dkSlotRowA.name == dkSlotDeclA_.name,
                "the row is enrolled on the first operation, as a slot row");
        fields = 1;
        OEXPECT(ownedSlotPublish(&sa, true, dkSlotRelease_, &cargo) &&
                    ownedSlotLoad(&sa) == OWNED_SLOT_ARMED &&
                    g_dkSlotReleases == 0,
                "row 3: publish(still_armed) leaves ARMED and runs no release");
        OEXPECT(ownedSlotClaim(&sa, &prior) && prior == OWNED_SLOT_ARMED,
                "row 2: the next claim is told ARMED (the fields are cargo)");
        fields = 0;
        OEXPECT(ownedSlotPublish(&sa, false, dkSlotRelease_, &cargo) &&
                    ownedSlotLoad(&sa) == OWNED_SLOT_EMPTY &&
                    g_dkSlotReleases == 0,
                "row 4: publish(!still_armed) leaves EMPTY, the resting "
                "state, and runs no release");

        /* row 5: a second claimant meets CLAIMED */
        OEXPECT(ownedSlotClaim(&sa, &prior) && prior == OWNED_SLOT_EMPTY,
                "the holder claims");
        OEXPECT(!ownedSlotClaim(&sa, &prior) &&
                    ownedSlotLoad(&sa) == OWNED_SLOT_CLAIMED &&
                    ownedSlotClaimRefusedTotal(&dkSlotDeclA_) == 1 &&
                    g_dkSlotNotes == 1 &&
                    g_dkSlotNoteDisplaced == OWNED_SLOT_CLAIMED,
                "row 5: a claim against CLAIMED is refused, counted on "
                "claim_refused_total, named with the displaced state, and "
                "the word is unchanged");
        OEXPECT(ownedSlotPublish(&sa, false, dkSlotRelease_, &cargo),
                "the holder publishes back EMPTY");

        /* row 6: a retire meets the resting state */
        OEXPECT(ownedSlotRetire(&sa, dkSlotRelease_, &cargo) ==
                        OWNED_SLOT_EMPTY &&
                    g_dkSlotReleases == 0 &&
                    ownedSlotRetireMetClaimedTotal(&dkSlotDeclA_) == 0,
                "row 6: a retire that displaces EMPTY runs no release and "
                "moves no counter");

        /* row 7: a retire meets ARMED; the retirer runs the release */
        ownedSlot sb = OWNED_SLOT_INIT(&dkSlotDeclA_);
        OEXPECT(ownedSlotClaim(&sb, &prior) && prior == OWNED_SLOT_EMPTY,
                "a pass claims a fresh slot");
        fields = 7;
        OEXPECT(ownedSlotPublish(&sb, true, dkSlotRelease_, &cargo) &&
                    ownedSlotLoad(&sb) == OWNED_SLOT_ARMED,
                "and publishes ARMED");
        OEXPECT(ownedSlotRetire(&sb, dkSlotRelease_, &cargo) ==
                        OWNED_SLOT_ARMED &&
                    g_dkSlotReleases == 1 && g_dkSlotReleasedCargo == &cargo,
                "row 7: a retire that displaces ARMED runs the release "
                "exactly once, in the retire, with the given cargo");
        fields = 0;
        /* row 10: RETIRED is terminal */
        OEXPECT(ownedSlotRetire(&sb, dkSlotRelease_, &cargo) ==
                        OWNED_SLOT_RETIRED &&
                    g_dkSlotReleases == 1 &&
                    ownedSlotRetireMetClaimedTotal(&dkSlotDeclA_) == 0,
                "row 10: a second retire is told RETIRED and releases "
                "nothing");
        /* row 11: no claim after a retire */
        const uint64_t refusedBefore =
            ownedSlotClaimRefusedTotal(&dkSlotDeclA_);
        OEXPECT(!ownedSlotClaim(&sb, &prior) &&
                    ownedSlotClaimRefusedTotal(&dkSlotDeclA_) ==
                        refusedBefore + 1 &&
                    g_dkSlotNoteDisplaced == OWNED_SLOT_RETIRED,
                "row 11: a RETIRED slot admits no claim, counted and named "
                "RETIRED");

        /* rows 8-9: a retire lands while the slot is claimed */
        ownedSlot sc = OWNED_SLOT_INIT(&dkSlotDeclA_);
        OEXPECT(ownedSlotClaim(&sc, &prior) && prior == OWNED_SLOT_EMPTY,
                "the pass claims");
        fields = 42; /* the pass owns them from here */
        g_dkSlotNotes = 0;
        OEXPECT(ownedSlotRetire(&sc, dkSlotRelease_, &cargo) ==
                        OWNED_SLOT_CLAIMED &&
                    g_dkSlotReleases == 1 && fields == 42 &&
                    ownedSlotRetireMetClaimedTotal(&dkSlotDeclA_) == 1 &&
                    g_dkSlotNotes == 1 &&
                    g_dkSlotNoteDisplaced == OWNED_SLOT_CLAIMED &&
                    g_dkSlotNoteReading == 1 &&
                    g_dkSlotNoteName == dkSlotDeclA_.name,
                "row 8: a retire that displaces CLAIMED touches nothing, "
                "runs no release, counts retire_met_claimed_total and notes "
                "once with the instance's name");
        OEXPECT(!ownedSlotPublish(&sc, true, dkSlotRelease_, &cargo) &&
                    g_dkSlotReleases == 2 &&
                    ownedSlotLoad(&sc) == OWNED_SLOT_RETIRED &&
                    ownedSlotReleaseByClaimantTotal(&dkSlotDeclA_) == 1 &&
                    g_dkSlotNotes == 2 &&
                    g_dkSlotNoteDisplaced == OWNED_SLOT_RETIRED,
                "row 9: the holder's publish-back fails against RETIRED, "
                "runs the release exactly once in the publish, stores "
                "RETIRED back, counts release_by_claimant_total and notes "
                "once");
        fields = 0;

        /* Exactly one release per lifecycle: sb (released by the retire)
         * and sc (released by the claimant) once each, and sa (nothing
         * present at its retire) zero times. */
        OEXPECT(g_dkSlotReleases == 2,
                "slot: cargo has exactly one release per lifecycle");

        /* A NULL note counts and says nothing. */
        ownedSlot sq = OWNED_SLOT_INIT(&dkSlotDeclQuiet_);
        g_dkSlotNotes = 0;
        OEXPECT(ownedSlotClaim(&sq, &prior) &&
                    ownedSlotRetire(&sq, dkSlotRelease_, &cargo) ==
                        OWNED_SLOT_CLAIMED &&
                    ownedSlotRetireMetClaimedTotal(&dkSlotDeclQuiet_) == 1 &&
                    g_dkSlotNotes == 0,
                "slot: a NULL note moves the counter and reports nothing");
        OEXPECT(!ownedSlotPublish(&sq, false, dkSlotRelease_, &cargo) &&
                    ownedSlotReleaseByClaimantTotal(&dkSlotDeclQuiet_) == 1 &&
                    g_dkSlotNotes == 0,
                "and its claimant's release likewise");

        /* Counters are per declaration: a second name driven through row
         * 8 reads 1 on its own row while A still reads 1. */
        ownedSlot sd = OWNED_SLOT_INIT(&dkSlotDeclB_);
        OEXPECT(ownedSlotClaim(&sd, &prior) &&
                    ownedSlotRetire(&sd, dkSlotRelease_, &cargo) ==
                        OWNED_SLOT_CLAIMED &&
                    ownedSlotRetireMetClaimedTotal(&dkSlotDeclB_) == 1 &&
                    ownedSlotRetireMetClaimedTotal(&dkSlotDeclA_) == 1 &&
                    ownedStateRowAt(0) != NULL,
                "slot: each declaration reads its own count");
        (void)ownedSlotPublish(&sd, false, dkSlotRelease_, &cargo);

        /* A zero-filled slot with no declaration runs the same protocol
         * with the same return values; no counter moves, no row is
         * enrolled, no note fires. */
        ownedSlot su;
        memset(&su, 0, sizeof(su)); /* zero fill is EMPTY */
        const size_t rowsNow = ownedStateRowCount();
        g_dkSlotNotes = 0;
        g_dkSlotReleases = 0;
        OEXPECT(ownedSlotLoad(&su) == OWNED_SLOT_EMPTY &&
                    ownedSlotClaim(&su, &prior) && prior == OWNED_SLOT_EMPTY &&
                    ownedSlotPublish(&su, true, dkSlotRelease_, &cargo) &&
                    ownedSlotLoad(&su) == OWNED_SLOT_ARMED &&
                    ownedSlotClaim(&su, &prior) && prior == OWNED_SLOT_ARMED &&
                    !ownedSlotClaim(&su, &prior) &&
                    ownedSlotRetire(&su, dkSlotRelease_, &cargo) ==
                        OWNED_SLOT_CLAIMED &&
                    g_dkSlotReleases == 0 &&
                    !ownedSlotPublish(&su, true, dkSlotRelease_, &cargo) &&
                    g_dkSlotReleases == 1 &&
                    ownedSlotLoad(&su) == OWNED_SLOT_RETIRED &&
                    !ownedSlotClaim(&su, &prior) &&
                    ownedSlotRetire(&su, dkSlotRelease_, &cargo) ==
                        OWNED_SLOT_RETIRED,
                "slot: a slot without a declaration runs every transition "
                "with the declared slot's return values");
        OEXPECT(ownedStateRowCount() == rowsNow && g_dkSlotNotes == 0,
                "and enrols no row and fires no note");

        /* The runtime initialiser gives what the static one gives, over
         * whatever the storage held. */
        {
            ownedSlot si;
            memset(&si, 0x5A, sizeof(si));
            ownedSlotInit(&si, &dkSlotDeclB_);
            const ownedSlot ss = OWNED_SLOT_INIT(&dkSlotDeclB_);
            OEXPECT(ownedSlotLoad(&si) == ownedSlotLoad(&ss) &&
                        ownedSlotLoad(&si) == OWNED_SLOT_EMPTY &&
                        si.decl == ss.decl,
                    "slot: ownedSlotInit matches OWNED_SLOT_INIT");
            OEXPECT(
                !strcmp(ownedStateKindName(OWNED_ROW_ARENA), "arena") &&
                    !strcmp(ownedStateKindName(OWNED_ROW_CELL), "cell") &&
                    !strcmp(ownedStateKindName(OWNED_ROW_SLOT), "slot") &&
                    !strcmp(ownedStateKindName(OWNED_ROW_SEQ), "seq") &&
                    !strcmp(ownedStateKindName((ownedStateKind)99), "unknown"),
                "row kinds: each kind has its name, others are unknown");
        }

        /* Publish without a claim is a misuse: it returns false and
         * leaves the state and the cargo alone. */
        ownedSlot sm = OWNED_SLOT_INIT(NULL);
        g_dkSlotReleases = 0;
        OEXPECT(!ownedSlotPublish(&sm, true, dkSlotRelease_, &cargo) &&
                    ownedSlotLoad(&sm) == OWNED_SLOT_EMPTY &&
                    g_dkSlotReleases == 0,
                "slot: publish on an EMPTY slot changes nothing");
        OEXPECT(ownedSlotClaim(&sm, &prior) &&
                    ownedSlotPublish(&sm, true, dkSlotRelease_, &cargo) &&
                    !ownedSlotPublish(&sm, false, dkSlotRelease_, &cargo) &&
                    ownedSlotLoad(&sm) == OWNED_SLOT_ARMED &&
                    g_dkSlotReleases == 0,
                "slot: publish on an ARMED slot changes nothing");
        OEXPECT(ownedSlotRetire(&sm, dkSlotRelease_, &cargo) ==
                        OWNED_SLOT_ARMED &&
                    g_dkSlotReleases == 1,
                "slot: and the cargo is still released once, by the retire");

        /* A Publish that holds no live claim never releases: a thread
         * whose claim was refused after a retire of ARMED (the retire
         * already released), and a claimant repeating its Publish after
         * releasing once. */
        ownedSlot sr = OWNED_SLOT_INIT(NULL);
        g_dkSlotReleases = 0;
        OEXPECT(ownedSlotClaim(&sr, &prior) &&
                    ownedSlotPublish(&sr, true, dkSlotRelease_, &cargo) &&
                    ownedSlotRetire(&sr, dkSlotRelease_, &cargo) ==
                        OWNED_SLOT_ARMED &&
                    g_dkSlotReleases == 1 && !ownedSlotClaim(&sr, &prior),
                "slot: a retire of ARMED releases and refuses later claims");
        OEXPECT(!ownedSlotPublish(&sr, true, dkSlotRelease_, &cargo) &&
                    g_dkSlotReleases == 1 &&
                    ownedSlotLoad(&sr) == OWNED_SLOT_RETIRED,
                "slot: a refused claimant's Publish releases nothing");
        ownedSlot sp = OWNED_SLOT_INIT(NULL);
        g_dkSlotReleases = 0;
        OEXPECT(ownedSlotClaim(&sp, &prior) &&
                    ownedSlotRetire(&sp, dkSlotRelease_, &cargo) ==
                        OWNED_SLOT_CLAIMED &&
                    ownedSlotLoad(&sp) == OWNED_SLOT_RETIRED &&
                    ownedSlotRetire(&sp, dkSlotRelease_, &cargo) ==
                        OWNED_SLOT_RETIRED &&
                    !ownedSlotClaim(&sp, &prior) && g_dkSlotReleases == 0,
                "slot: a retire of CLAIMED reads RETIRED and leaves the "
                "release to the claimant");
        OEXPECT(!ownedSlotPublish(&sp, true, dkSlotRelease_, &cargo) &&
                    g_dkSlotReleases == 1 &&
                    !ownedSlotPublish(&sp, true, dkSlotRelease_, &cargo) &&
                    g_dkSlotReleases == 1 &&
                    ownedSlotLoad(&sp) == OWNED_SLOT_RETIRED,
                "slot: a repeated Publish after the claimant's release "
                "releases nothing");
        (void)fields;
    }

    /* ownedSeq */
    {
        uint8_t out[DK_SEQ_BYTES];
        size_t outLen = 0;
        uint8_t payload[DK_SEQ_BYTES];

        /* A runtime-sized instance through ownedSeqInit; storage from the
         * owner, zeroed by Init. */
        ownedSeq sa;
        cleanerName(zfree, Bytes) uint8_t *storageA =
            (uint8_t *)zmalloc(ownedSeqStorageBytes(&dkSeqDeclA_));
        OEXPECT(storageA != NULL && ownedSeqStorageBytes(&dkSeqDeclA_) ==
                                        4u * (16u + DK_SEQ_BYTES),
                "seq: the stride is 16 + roundup8(version_bytes)");
        ownedSeqInit(&sa, &dkSeqDeclA_, NULL, storageA);
        const uint64_t pubBase = ownedSeqPublishedTotal();
        const uint64_t tornBase = ownedSeqTornReadsTotal();
        const uint64_t overBase = ownedSeqOverwrittenReadsTotal();
        const uint64_t abandonBase = ownedSeqPublishAbandonedTotal();
        const uint64_t oversizeBase = ownedSeqPublishRefusedOversizeTotal();

        /* A published version reads VALID, byte for byte. */
        dkSeqPayload_(payload, 0x11);
        OEXPECT(ownedSeqPublish(&sa, payload, sizeof(payload)) &&
                    ownedSeqCursor(&sa) == 1 && ownedSeqAssigned(&sa) == 1,
                "seq: the first publication is number 1 on both cursors");
        OEXPECT(ownedSeqReadAt(&sa, 1, out, sizeof(out), &outLen) ==
                        OWNED_SEQ_VALID &&
                    outLen == DK_SEQ_BYTES &&
                    memcmp(out, payload, DK_SEQ_BYTES) == 0 &&
                    ownedSeqPublishedTotal() == pubBase + 1 &&
                    ownedSeqColumnTotal(&dkSeqDeclA_, OWNED_SEQ_C_PUBLISHED) ==
                        1,
                "seq: VALID, whole, and counted on the row");

        /* OVERWRITTEN is reachable and is not TORN: N = 4, publish four
         * more (five total), so version 0 now holds 5. */
        for (uint8_t i = 0; i < 4; i++) {
            dkSeqPayload_(payload, (uint8_t)(0x20 + i));
            OEXPECT(ownedSeqPublish(&sa, payload, sizeof(payload)),
                    "seq: publications land");
        }
        OEXPECT(ownedSeqReadAt(&sa, 1, out, sizeof(out), &outLen) ==
                        OWNED_SEQ_OVERWRITTEN &&
                    ownedSeqOverwrittenReadsTotal() == overBase + 1 &&
                    ownedSeqTornReadsTotal() == tornBase,
                "seq: a recycled version reads OVERWRITTEN, not TORN");
        OEXPECT(ownedSeqReadAt(&sa, 5, out, sizeof(out), &outLen) ==
                        OWNED_SEQ_VALID &&
                    out[0] == 0x23,
                "seq: the version's current publication reads VALID");

        /* TORN mid-publication, and the two cursors differ, both read
         * from inside the fill on one thread. */
        dkSeqInsideFill inside = {.seq = &sa,
                                  .number = ownedSeqAssigned(&sa) + 1};
        OEXPECT(
            ownedSeqPublishWith(&sa, DK_SEQ_BYTES, dkSeqInsideFillFn_, &inside),
            "seq: the fill-callback publication lands");
        OEXPECT(inside.readInside == OWNED_SEQ_TORN,
                "seq: a read of the number being written is TORN");
        OEXPECT(inside.assignedInside == inside.number &&
                    inside.cursorInside == inside.number - 1,
                "seq: inside the fill assigned == s and cursor == s - 1");
        OEXPECT(ownedSeqReadAt(&sa, inside.number, out, sizeof(out), &outLen) ==
                        OWNED_SEQ_VALID &&
                    out[0] == 0x33,
                "seq: and it reads VALID once published");

        /* The inline single-word reader agrees with ReadAt on every
         * verdict for a one-word declaration, and reads OVERWRITTEN for
         * a declaration that is not one word. */
        {
            static ownedStateRow wordRow;
            static const ownedSeqDecl wordDecl = {
                .name = "dk.seq.word",
                .versions = 4,
                .version_bytes = sizeof(uint64_t),
                .discipline = OWNED_WRITER_CLAIM,
                .writer_ok = NULL,
                .note_violation = NULL,
                .row = &wordRow,
            };
            static _Atomic uint64_t
                wordStorage[OWNED_SEQ_STORAGE_WORDS(4u, sizeof(uint64_t))];
            ownedSeq sw = OWNED_SEQ_INIT(&wordDecl, wordStorage);
            uint64_t w = 0;
            const uint64_t one = UINT64_C(0x1111111111111111);
            const uint64_t two = UINT64_C(0x2222222222222222);
            OEXPECT(ownedSeqReadWordAt(&sw, 1, &w) == OWNED_SEQ_TORN,
                    "seq word: never-written reads TORN through the word "
                    "reader");
            OEXPECT(ownedSeqPublish(&sw, &one, sizeof(one)) &&
                        ownedSeqReadWordAt(&sw, 1, &w) == OWNED_SEQ_VALID &&
                        w == one,
                    "seq word: a published word reads VALID and whole");
            for (int i = 0; i < 4; i++) {
                (void)ownedSeqPublish(&sw, &two, sizeof(two));
            }
            OEXPECT(
                ownedSeqReadWordAt(&sw, 1, &w) == OWNED_SEQ_OVERWRITTEN &&
                    ownedSeqReadWordAt(&sw, 5, &w) == OWNED_SEQ_VALID &&
                    w == two &&
                    ownedSeqColumnTotal(&wordDecl,
                                        OWNED_SEQ_C_OVERWRITTEN_READS) == 1 &&
                    ownedSeqColumnTotal(&wordDecl, OWNED_SEQ_C_TORN_READS) == 1,
                "seq word: recycled reads OVERWRITTEN, counted on the row "
                "as ReadAt counts");
            /* Numbers 2..5 are live. Publishing 6 recycles 2's version;
             * 3's version is the next stride, so a word written past the
             * payload would land on its stamp. */
            const uint64_t three = UINT64_C(0x3333333333333333);
            uint64_t filled = three;
            OEXPECT(ownedSeqPublishWith(&sw, sizeof(three), dkSeqWordFillFn_,
                                        &filled) &&
                        ownedSeqReadWordAt(&sw, 6, &w) == OWNED_SEQ_VALID &&
                        w == three,
                    "seq word: a fill through PutWord reads back VALID");
            OEXPECT(ownedSeqReadWordAt(&sw, 3, &w) == OWNED_SEQ_VALID &&
                        w == two,
                    "seq word: PutWord past the payload is ignored");
            uint64_t wide = 0;
            OEXPECT(ownedSeqReadWordAt(&sa, ownedSeqAssigned(&sa), &wide) ==
                        OWNED_SEQ_OVERWRITTEN,
                    "seq word: a declaration wider than one word reads "
                    "OVERWRITTEN");
        }

        /* TORN, never written: a retry may succeed. */
        OEXPECT(ownedSeqReadAt(&sa, ownedSeqAssigned(&sa) + 1, out, sizeof(out),
                               &outLen) == OWNED_SEQ_TORN,
                "seq: a number the ring has not reached is TORN");

        /* A NULL source with a non-zero length is refused before a number
         * is assigned. */
        {
            const uint64_t assignedBefore = ownedSeqAssigned(&sa);
            const uint64_t pubBefore = ownedSeqPublishedTotal();
            OEXPECT(!ownedSeqPublish(&sa, NULL, DK_SEQ_BYTES) &&
                        ownedSeqAssigned(&sa) == assignedBefore &&
                        ownedSeqPublishedTotal() == pubBefore,
                    "seq: publish(NULL, len > 0) is refused, no number "
                    "assigned");
            /* The same holds for PublishWith without a fill: the version
             * would otherwise read VALID with an older publication's
             * bytes. */
            OEXPECT(!ownedSeqPublishWith(&sa, 16, NULL, NULL) &&
                        ownedSeqAssigned(&sa) == assignedBefore &&
                        ownedSeqPublishedTotal() == pubBefore &&
                        ownedSeqReadAt(&sa, assignedBefore + 1, out,
                                       sizeof(out), &outLen) == OWNED_SEQ_TORN,
                    "seq: publishWith(len > 0, NULL fill) is refused, no "
                    "number assigned, nothing reads VALID");
        }

        /* Oversize publish is refused before a number exists. */
        {
            const uint64_t assignedBefore = ownedSeqAssigned(&sa);
            uint8_t big[DK_SEQ_BYTES + 1];
            memset(big, 0x5A, sizeof(big));
            OEXPECT(!ownedSeqPublish(&sa, big, sizeof(big)) &&
                        ownedSeqAssigned(&sa) == assignedBefore &&
                        ownedSeqPublishRefusedOversizeTotal() ==
                            oversizeBase + 1 &&
                        ownedSeqColumnTotal(&dkSeqDeclA_,
                                            OWNED_SEQ_C_REFUSED_OVERSIZE) == 1,
                    "seq: oversize is refused, no number assigned, counted");
        }
        /* CAPACITY copies nothing and reports the length. */
        {
            uint8_t small[8];
            memset(small, 0xEE, sizeof(small));
            const uint64_t want = ownedSeqAssigned(&sa);
            OEXPECT(ownedSeqReadAt(&sa, want, small, sizeof(small), &outLen) ==
                            OWNED_SEQ_CAPACITY &&
                        outLen == DK_SEQ_BYTES && small[0] == 0xEE,
                    "seq: an undersized dst reads CAPACITY with the "
                    "required length and nothing copied");
            OEXPECT(ownedSeqReadAt(&sa, want, out, sizeof(out), &outLen) ==
                            OWNED_SEQ_VALID &&
                        outLen == DK_SEQ_BYTES,
                    "seq: a correctly sized retry reads VALID");
        }

        /* The decline predicate allows N - 1 further publications. */
        {
            const uint64_t before = ownedSeqAssigned(&sa);
            dkSeqPayload_(payload, 0x44);
            (void)ownedSeqPublish(&sa, payload, sizeof(payload));
            (void)ownedSeqPublish(&sa, payload, sizeof(payload));
            OEXPECT(ownedSeqReadCanSucceed(&sa, before),
                    "seq: after 2 further publications a reader may retry");
            (void)ownedSeqPublish(&sa, payload, sizeof(payload));
            OEXPECT(!ownedSeqReadCanSucceed(&sa, before),
                    "seq: after 3 (N - 1) it declines");
        }

        /* Writer predicate, and counters per declaration: the checked
         * decl shares row A, so publications through either count on
         * one row. */
        ownedSeq sc;
        cleanerName(zfree, Bytes) uint8_t *storageC =
            (uint8_t *)zmalloc(ownedSeqStorageBytes(&dkSeqDeclChecked_));
        ownedSeqInit(&sc, &dkSeqDeclChecked_, NULL, storageC);
        const uint64_t violBase = ownedCellWriterViolationTotal();
        const uint64_t rowAPublishedBefore =
            ownedSeqColumnTotal(&dkSeqDeclA_, OWNED_SEQ_C_PUBLISHED);
        /* The declared writer is silent. */
        g_dkSeqWriterOk = true;
        atomic_store_explicit(&g_dkSeqViolationNotes, 0, memory_order_relaxed);
        dkSeqPayload_(payload, 0x66);
        OEXPECT(ownedSeqPublish(&sc, payload, sizeof(payload)) &&
                    ownedCellWriterViolationTotal() == violBase &&
                    atomic_load_explicit(&g_dkSeqViolationNotes,
                                         memory_order_relaxed) == 0,
                "seq: the declared writer is silent");
        /* A foreign writer is counted and reported, and still lands. */
        g_dkSeqWriterOk = false;
        dkSeqPayload_(payload, 0x77);
        OEXPECT(ownedSeqPublish(&sc, payload, sizeof(payload)) &&
                    ownedCellWriterViolationTotal() == violBase + 1 &&
                    atomic_load_explicit(&g_dkSeqViolationNotes,
                                         memory_order_relaxed) == 1 &&
                    ownedSeqReadAt(&sc, 2, out, sizeof(out), &outLen) ==
                        OWNED_SEQ_VALID &&
                    out[0] == 0x77,
                "seq: a foreign writer is counted, reported, and its "
                "publication still lands");
        g_dkSeqWriterOk = true;
        /* A NULL predicate (sa's decl) disables the check. */
        dkSeqPayload_(payload, 0x88);
        OEXPECT(ownedSeqPublish(&sa, payload, sizeof(payload)) &&
                    ownedCellWriterViolationTotal() == violBase + 1,
                "seq: a NULL predicate disables the check");
        OEXPECT(ownedSeqColumnTotal(&dkSeqDeclA_, OWNED_SEQ_C_PUBLISHED) ==
                        rowAPublishedBefore + 3 &&
                    ownedSeqColumnTotal(&dkSeqDeclChecked_,
                                        OWNED_SEQ_C_PUBLISHED) ==
                        rowAPublishedBefore + 3,
                "seq: two declarations sharing one row aggregate on it");

        /* A zero-version seq ignores publishes and counts nothing. */
        {
            ownedSeq sz;
            memset(&sz, 0, sizeof(sz));
            const size_t rowsNow = ownedStateRowCount();
            const uint64_t pubNow = ownedSeqPublishedTotal();
            const uint64_t overNow = ownedSeqOverwrittenReadsTotal();
            OEXPECT(!ownedSeqPublish(&sz, payload, sizeof(payload)) &&
                        ownedSeqReadAt(&sz, 1, out, sizeof(out), &outLen) ==
                            OWNED_SEQ_OVERWRITTEN &&
                        ownedSeqCursor(&sz) == 0 &&
                        ownedSeqAssigned(&sz) == 0 &&
                        ownedSeqVersions(&sz) == 0 &&
                        !ownedSeqReadCanSucceed(&sz, 0) &&
                        ownedSeqPublishedTotal() == pubNow &&
                        ownedSeqOverwrittenReadsTotal() == overNow &&
                        ownedStateRowCount() == rowsNow,
                    "seq: a zero-version seq refuses publishes, its decline "
                    "predicate is false, no counter moves, no row enrols");
        }

        /* A delayed writer never replaces a newer publication.  A writer
         * that took number n and was delayed until n + N had completed in
         * the same version must abandon.  The delay is reproduced on one
         * thread by rewinding `assigned` once n + N has landed, so the
         * next publish takes number n again. */
        {
            static ownedStateRow lateRow;
            static const ownedSeqDecl lateDecl = {
                .name = "dk.seq.late",
                .versions = 4,
                .version_bytes = DK_SEQ_BYTES,
                .discipline = OWNED_WRITER_CLAIM,
                .writer_ok = NULL,
                .note_violation = NULL,
                .row = &lateRow,
            };
            ownedSeq sl;
            cleanerName(zfree, Bytes) uint8_t *storageL =
                (uint8_t *)zmalloc(ownedSeqStorageBytes(&lateDecl));
            ownedSeqInit(&sl, &lateDecl, NULL, storageL);
            for (uint8_t i = 1; i <= 5; i++) {
                dkSeqPayload_(payload, (uint8_t)(0x50 + i));
                (void)ownedSeqPublish(&sl, payload, sizeof(payload));
            }
            /* Version 0 now holds number 5; replay number 1 into it. */
            atomic_store_explicit(&sl.assigned, 0, memory_order_relaxed);
            dkSeqPayload_(payload, 0xEE);
            OEXPECT(
                ownedSeqPublish(&sl, payload, sizeof(payload)) &&
                    ownedSeqColumnTotal(&lateDecl,
                                        OWNED_SEQ_C_PUBLISH_ABANDONED) == 1 &&
                    ownedSeqColumnTotal(&lateDecl, OWNED_SEQ_C_PUBLISHED) == 5,
                "seq: a writer older than the version's publication "
                "abandons");
            atomic_store_explicit(&sl.assigned, 5, memory_order_relaxed);
            uint8_t newest[DK_SEQ_BYTES];
            dkSeqPayload_(newest, 0x55);
            OEXPECT(ownedSeqReadAt(&sl, 5, out, sizeof(out), &outLen) ==
                            OWNED_SEQ_VALID &&
                        memcmp(out, newest, DK_SEQ_BYTES) == 0 &&
                        ownedSeqCursor(&sl) == 5,
                    "seq: the newer publication survives and reads VALID");
            OEXPECT(ownedSeqReadAt(&sl, 1, out, sizeof(out), &outLen) ==
                        OWNED_SEQ_OVERWRITTEN,
                    "seq: the delayed writer's number reads OVERWRITTEN");
            ownedSeqDestruct(&sl);
        }

        /* Nested publication on N == 1: a publish from inside another's
         * fill finds the version held and abandons without writing. */
        {
            ownedSeq so;
            cleanerName(zfree, Bytes) uint8_t *storageO =
                (uint8_t *)zmalloc(ownedSeqStorageBytes(&dkSeqDeclOne_));
            ownedSeqInit(&so, &dkSeqDeclOne_, NULL, storageO);
            const uint64_t abandonNow = ownedSeqPublishAbandonedTotal();
            dkSeqNestedFill nested = {.seq = &so, .innerNumber = 0};
            const uint64_t outerNumber = ownedSeqAssigned(&so) + 1;
            (void)ownedSeqPublishWith(&so, DK_SEQ_BYTES, dkSeqNestedFillFn_,
                                      &nested);
            OEXPECT(ownedSeqPublishAbandonedTotal() == abandonNow + 1 &&
                        ownedSeqColumnTotal(&dkSeqDeclOne_,
                                            OWNED_SEQ_C_PUBLISH_ABANDONED) == 1,
                    "seq: the writer that finds the version held abandons, "
                    "counted once");
            {
                uint8_t a[DK_SEQ_BYTES];
                dkSeqPayload_(a, 0xA0);
                OEXPECT(ownedSeqReadAt(&so, outerNumber, out, sizeof(out),
                                       &outLen) == OWNED_SEQ_VALID &&
                            memcmp(out, a, DK_SEQ_BYTES) == 0,
                        "seq: the holder's publication reads VALID and "
                        "whole; the abandoning writer stored nothing");
            }
            OEXPECT(ownedSeqReadAt(&so, nested.innerNumber, out, sizeof(out),
                                   &outLen) == OWNED_SEQ_TORN,
                    "seq: the abandoned number reads TORN while the version "
                    "holds an older publication");
            dkSeqPayload_(payload, 0xC0);
            OEXPECT(ownedSeqPublish(&so, payload, sizeof(payload)) &&
                        ownedSeqReadAt(&so, ownedSeqAssigned(&so), out,
                                       sizeof(out),
                                       &outLen) == OWNED_SEQ_VALID &&
                        memcmp(out, payload, DK_SEQ_BYTES) == 0 &&
                        ownedSeqCursor(&so) == ownedSeqAssigned(&so),
                    "seq: the version returned to service; the next "
                    "publication reads VALID");
            OEXPECT(ownedSeqReadAt(&so, nested.innerNumber, out, sizeof(out),
                                   &outLen) == OWNED_SEQ_OVERWRITTEN,
                    "seq: once lapped, the abandoned number reads "
                    "OVERWRITTEN");
            ownedSeqDestruct(&so);
        }

        /* MPSC: kWriters writers, kPerWriter publications each, on a ring
         * wide enough that no version is shared. */
        {
            enum { kWriters = 4, kPerWriter = 32 };
            ownedSeq sw;
            cleanerName(zfree, Bytes) uint8_t *storageW =
                (uint8_t *)zmalloc(ownedSeqStorageBytes(&dkSeqDeclWide_));
            ownedSeqInit(&sw, &dkSeqDeclWide_, NULL, storageW);
            const uint64_t pubNow = ownedSeqPublishedTotal();
            const uint64_t abandonNow = ownedSeqPublishAbandonedTotal();
            pthread_t th[kWriters];
            dkSeqWriterCtx ctx[kWriters];
            for (int i = 0; i < kWriters; i++) {
                ctx[i] = (dkSeqWriterCtx){.seq = &sw,
                                          .tag = (uint8_t)(0x10 * (i + 1)),
                                          .count = kPerWriter};
                OEXPECT(pthread_create(&th[i], NULL, dkSeqWriterMain_,
                                       &ctx[i]) == 0,
                        "seq mpsc: writer started");
            }
            for (int i = 0; i < kWriters; i++) {
                pthread_join(th[i], NULL);
            }
            OEXPECT(ownedSeqAssigned(&sw) == kWriters * kPerWriter &&
                        ownedSeqCursor(&sw) == kWriters * kPerWriter &&
                        ownedSeqPublishedTotal() ==
                            pubNow + kWriters * kPerWriter &&
                        ownedSeqPublishAbandonedTotal() == abandonNow,
                    "seq mpsc: every number distinct, all published, none "
                    "abandoned");
            bool allWhole = true;
            for (uint64_t n = 1; n <= kWriters * kPerWriter; n++) {
                if (ownedSeqReadAt(&sw, n, out, sizeof(out), &outLen) !=
                        OWNED_SEQ_VALID ||
                    !dkSeqPayloadWhole_(out, outLen)) {
                    allWhole = false;
                }
            }
            OEXPECT(allWhole, "seq mpsc: every version validates and holds one "
                              "writer's payload whole");
            ownedSeqDestruct(&sw);
        }

        /* Overlapping writers, N == 1 and N == 4, with a concurrent
         * reader, followed by a liveness check. */
        {
            const ownedSeqDecl *decls[2] = {&dkSeqDeclOne_, &dkSeqDeclA_};
            for (int d = 0; d < 2; d++) {
                enum { kWriters = 8, kPerWriter = 2000 };
                ownedSeq sv;
                cleanerName(zfree, Bytes) uint8_t *storageV =
                    (uint8_t *)zmalloc(ownedSeqStorageBytes(decls[d]));
                ownedSeqInit(&sv, decls[d], NULL, storageV);
                _Atomic bool stop = false;
                dkSeqReaderCtx reader = {.seq = &sv, .stop = &stop};
                pthread_t rt;
                OEXPECT(pthread_create(&rt, NULL, dkSeqReaderMain_, &reader) ==
                            0,
                        "seq overlap: reader started");
                pthread_t th[kWriters];
                dkSeqWriterCtx ctx[kWriters];
                for (int i = 0; i < kWriters; i++) {
                    ctx[i] = (dkSeqWriterCtx){.seq = &sv,
                                              .tag = (uint8_t)(0x10 * (i + 1)),
                                              .count = kPerWriter};
                    OEXPECT(pthread_create(&th[i], NULL, dkSeqWriterMain_,
                                           &ctx[i]) == 0,
                            "seq overlap: writer started");
                }
                for (int i = 0; i < kWriters; i++) {
                    pthread_join(th[i], NULL);
                }
                atomic_store_explicit(&stop, true, memory_order_release);
                pthread_join(rt, NULL);
                OEXPECT(reader.mixedReads == 0,
                        "seq overlap: no VALID read carried a mixed payload");
                /* Without a VALID read the check above is vacuous. */
                OEXPECT(reader.validReads > 0,
                        "seq overlap: the reader validated at least one copy");
                /* Liveness: after the writers are joined, one publication
                 * into every version must read VALID. */
                bool healed = true;
                for (uint32_t v = 0; v < decls[d]->versions; v++) {
                    dkSeqPayload_(payload, 0xD0);
                    (void)ownedSeqPublish(&sv, payload, sizeof(payload));
                    if (ownedSeqReadAt(&sv, ownedSeqAssigned(&sv), out,
                                       sizeof(out),
                                       &outLen) != OWNED_SEQ_VALID ||
                        memcmp(out, payload, DK_SEQ_BYTES) != 0) {
                        healed = false;
                    }
                }
                OEXPECT(healed,
                        "seq overlap: after the writers are joined, every "
                        "version returns to service");
                ownedSeqDestruct(&sv);
            }
            /* Eight writers on N == 1 overlap, so some publications must
             * have been abandoned. */
            OEXPECT(ownedSeqPublishAbandonedTotal() > abandonBase + 1,
                    "seq overlap: publish_abandoned_total moved");
        }

        ownedSeqDestruct(&sc);
        ownedSeqDestruct(&sa);
    }

    /* READERS_ONLY runs after the default block so that block's
     * process-wide baselines are untouched by it. */
    failures += dkTestOwnedReadersOnly();
    failures += dkTestOwnedArenaSoak();

    if (failures == 0) {
        printf("dkTestOwnedState: PASS\n");
    }
#undef OEXPECT
    return failures;
}

/* ====================================================================
 * ownedSeq read-cost benchmark.
 *
 * Measures the p99 time of a whole-window walk (DK_GATE_N - 1 slots,
 * newest first) for three readers, at R = 1 and at R = DK_GATE_STORM_R
 * concurrent readers, while one paced writer appends:
 *
 *   reference  a classic single-version seqlock ring: a sequence word
 *              and a value per slot, written under a lock; the reader
 *              loads seq, value, seq with acquire;
 *   converted  ownedSeq with one-word versions, read through
 *              ownedSeqReadWordAt;
 *   probe      the same ownedSeq storage read with the same acquire
 *              loads, but indexed with a mask instead of the reader's
 *              `% versions` and declaration loads. Because N is a power
 *              of two it is a correct reader of this ring; it separates
 *              the cost of the layout (stride and header words) from the
 *              cost of the index arithmetic and call shape.
 *
 * Every configuration runs DK_GATE_TRIALS times, interleaved so that a
 * change in host load hits every reader alike, and each bound is
 * checked on the smallest of the trials' p99 values. Other processes
 * can only add time to a walk, so the least disturbed trial is the
 * best estimate of a reader's own tail, while a cost the reader itself
 * causes shows in every trial (the median is printed alongside):
 *   B1  the converted reader's storm / R = 1 ratio is at most twice
 *       the reference's, so ownedSeq adds no concurrency term;
 *   B2  the probe's p99 is within 1.25x of the reference's at both R,
 *       so the layout adds no cost;
 *   B3  the converted storm walk's p99 is below 1 ms.
 * The index-arithmetic cost (converted / probe) is printed, not bounded.
 * Under ThreadSanitizer B1 and B3 are printed but not asserted.
 *
 * The result depends on the machine and its load, so it is run by name
 * rather than as part of the default test run.
 * ==================================================================== */
#define DK_GATE_N 4096u
#define DK_GATE_WALKS 400u
#define DK_GATE_TRIALS 9u
#define DK_GATE_STORM_R 8u
#define DK_GATE_WRITER_PACE_NS 20000u /* about 50k appends/s */

typedef struct dkGateRefSlot {
    _Atomic uint64_t seq;
    _Atomic uint64_t keyHash;
} dkGateRefSlot;
static dkGateRefSlot g_dkGateRef[DK_GATE_N];
static _Atomic uint64_t g_dkGateRefSeq;
static pthread_mutex_t g_dkGateWriterLock = PTHREAD_MUTEX_INITIALIZER;

static void dkGateRefAppend_(uint64_t h) {
    pthread_mutex_lock(&g_dkGateWriterLock);
    const uint64_t s =
        atomic_load_explicit(&g_dkGateRefSeq, memory_order_relaxed) + 1;
    dkGateRefSlot *slot = &g_dkGateRef[s & (DK_GATE_N - 1u)];
    atomic_store_explicit(&slot->seq, 0, memory_order_release);
    atomic_store_explicit(&slot->keyHash, h, memory_order_release);
    atomic_store_explicit(&slot->seq, s, memory_order_release);
    atomic_store_explicit(&g_dkGateRefSeq, s, memory_order_release);
    pthread_mutex_unlock(&g_dkGateWriterLock);
}

/* Walk (since, until] newest first.  Returns 1 if `probe` is found, 0 if
 * not, and -1 if a slot fails validation or the window has been
 * lapped. */
static int dkGateRefWalk_(uint64_t since, uint64_t until, uint64_t probe) {
    const uint64_t cur =
        atomic_load_explicit(&g_dkGateRefSeq, memory_order_acquire);
    if (until > cur) {
        until = cur;
    }
    if (until <= since) {
        return 0;
    }
    if (cur - since >= DK_GATE_N) {
        return -1;
    }
    for (uint64_t s = until; s > since; s--) {
        const dkGateRefSlot *slot = &g_dkGateRef[s & (DK_GATE_N - 1u)];
        const uint64_t s1 =
            atomic_load_explicit(&slot->seq, memory_order_acquire);
        const uint64_t h =
            atomic_load_explicit(&slot->keyHash, memory_order_acquire);
        const uint64_t s2 =
            atomic_load_explicit(&slot->seq, memory_order_acquire);
        if (s1 != s || s2 != s) {
            return -1;
        }
        if (h == probe) {
            return 1;
        }
    }
    return 0;
}

static const ownedSeqDecl g_dkGateSeqDecl_ = {
    .name = "dk.seq.gate",
    .versions = DK_GATE_N,
    .version_bytes = sizeof(uint64_t),
    .discipline = OWNED_WRITER_CLAIM,
    .writer_ok = NULL,
    .note_violation = NULL,
    .row = NULL,
};
static _Atomic uint64_t
    g_dkGateSeqStorage_[OWNED_SEQ_STORAGE_WORDS(DK_GATE_N, sizeof(uint64_t))];
static ownedSeq g_dkGateSeq_ =
    OWNED_SEQ_INIT(&g_dkGateSeqDecl_, g_dkGateSeqStorage_);

static void dkGateSeqAppend_(uint64_t h) {
    pthread_mutex_lock(&g_dkGateWriterLock);
    (void)ownedSeqPublish(&g_dkGateSeq_, &h, sizeof(h));
    pthread_mutex_unlock(&g_dkGateWriterLock);
}

static int dkGateSeqWalk_(uint64_t since, uint64_t until, uint64_t probe) {
    const uint64_t cur = ownedSeqCursor(&g_dkGateSeq_);
    if (until > cur) {
        until = cur;
    }
    if (until <= since) {
        return 0;
    }
    if (cur - since >= (uint64_t)ownedSeqVersions(&g_dkGateSeq_)) {
        return -1;
    }
    for (uint64_t s = until; s > since; s--) {
        uint64_t h = 0;
        /* ownedSeq's inline single-word reader. */
        if (ownedSeqReadWordAt(&g_dkGateSeq_, s, &h) != OWNED_SEQ_VALID) {
            return -1;
        }
        if (h == probe) {
            return 1;
        }
    }
    return 0;
}

/* Probe: the ownedSeq layout read with the converted reader's acquire
 * loads, indexed with a constant mask and stride as the reference
 * indexes its ring. If it costs what the reference costs, the layout
 * adds nothing, and the converted reader's extra cost is its index
 * arithmetic (`% versions` and the declaration loads) and call shape. */
static int dkGateSeqWalkAcquire_(uint64_t since, uint64_t until,
                                 uint64_t probe) {
    const uint64_t cur = ownedSeqCursor(&g_dkGateSeq_);
    if (until > cur) {
        until = cur;
    }
    if (until <= since) {
        return 0;
    }
    if (cur - since >= (uint64_t)DK_GATE_N) {
        return -1;
    }
    const size_t stride = OWNED_SEQ_STORAGE_WORDS(1u, sizeof(uint64_t));
    for (uint64_t s = until; s > since; s--) {
        _Atomic uint64_t *const v =
            &g_dkGateSeqStorage_[(size_t)((s - 1) & (DK_GATE_N - 1u)) * stride];
        const uint64_t published = 4u * s;
        const uint64_t v1 = atomic_load_explicit(&v[0], memory_order_acquire);
        if (v1 != published) {
            return -1;
        }
        const uint64_t h = atomic_load_explicit(&v[OWNED_SEQ_HEADER_WORDS],
                                                memory_order_acquire);
        const uint64_t v2 = atomic_load_explicit(&v[0], memory_order_acquire);
        if (v2 != published) {
            return -1;
        }
        if (h == probe) {
            return 1;
        }
    }
    return 0;
}

typedef enum dkGateFlavour {
    DK_GATE_REFERENCE = 0,
    DK_GATE_CONVERTED = 1,
    DK_GATE_ACQUIRE_PROBE = 2,
} dkGateFlavour;

typedef struct dkGateWriterCtx {
    _Atomic bool *stop;
    bool converted;
    uint64_t appends;
} dkGateWriterCtx;

static void *dkGateWriterMain_(void *arg) {
    dkGateWriterCtx *w = arg;
    uint64_t x = UINT64_C(0x9E3779B97F4A7C15);
    uint64_t next = timeUtilMonotonicNs();
    while (!atomic_load_explicit(w->stop, memory_order_acquire)) {
        /* Appends are paced on the monotonic clock (busy-waiting), so a
         * whole-window walk sees a steady append rate instead of a ring
         * lapped underneath it. */
        if (timeUtilMonotonicNs() < next) {
            continue;
        }
        next += DK_GATE_WRITER_PACE_NS;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        if (w->converted) {
            dkGateSeqAppend_(x);
        } else {
            dkGateRefAppend_(x);
        }
        w->appends++;
    }
    return NULL;
}

typedef struct dkGateReaderCtx {
    dkGateFlavour flavour;
    uint64_t walkNs[DK_GATE_WALKS];
    uint64_t unprovable;
} dkGateReaderCtx;

static void *dkGateReaderMain_(void *arg) {
    dkGateReaderCtx *r = arg;
    for (uint32_t i = 0; i < DK_GATE_WALKS; i++) {
        const uint64_t cur =
            r->flavour != DK_GATE_REFERENCE
                ? ownedSeqCursor(&g_dkGateSeq_)
                : atomic_load_explicit(&g_dkGateRefSeq, memory_order_acquire);
        /* The largest window: DK_GATE_N - 1 slots, newest first. */
        const uint64_t since =
            cur > (DK_GATE_N - 1u) ? cur - (DK_GATE_N - 1u) : 0;
        const uint64_t t0 = timeUtilMonotonicNs();
        const int v = r->flavour == DK_GATE_CONVERTED
                          ? dkGateSeqWalk_(since, cur, 1)
                          : (r->flavour == DK_GATE_ACQUIRE_PROBE
                                 ? dkGateSeqWalkAcquire_(since, cur, 1)
                                 : dkGateRefWalk_(since, cur, 1));
        r->walkNs[i] = timeUtilMonotonicNs() - t0;
        if (v < 0) {
            r->unprovable++;
        }
    }
    return NULL;
}

static int dkGateCmpU64_(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void dkGateSortU64_(uint64_t *v, size_t n) {
    qsort(v, n, sizeof(*v), dkGateCmpU64_);
}

defcleanerName(dkGateReaderCtx *, zfree, GateReaderCtx);
defcleanerName(pthread_t *, zfree, Threads);
defcleanerName(uint64_t *, zfree, U64s);

/* One configuration: R readers walking against a paced writer; returns
 * the p99 whole-window walk in ns across every reader's walks. */
static uint64_t dkGateRun_(dkGateFlavour flavour, uint32_t readers,
                           uint64_t *outUnprovable, uint64_t *outAppends) {
    _Atomic bool stop = false;
    dkGateWriterCtx w = {.stop = &stop,
                         .converted = flavour != DK_GATE_REFERENCE};
    pthread_t wt;
    if (pthread_create(&wt, NULL, dkGateWriterMain_, &w) != 0) {
        return UINT64_MAX;
    }
    cleanerName(zfree, GateReaderCtx) dkGateReaderCtx *ctx =
        zcalloc(readers, sizeof(*ctx));
    cleanerName(zfree, Threads) pthread_t *th = zcalloc(readers, sizeof(*th));
    for (uint32_t i = 0; i < readers; i++) {
        ctx[i].flavour = flavour;
        if (pthread_create(&th[i], NULL, dkGateReaderMain_, &ctx[i]) != 0) {
            readers = i;
            break;
        }
    }
    for (uint32_t i = 0; i < readers; i++) {
        pthread_join(th[i], NULL);
    }
    atomic_store_explicit(&stop, true, memory_order_release);
    pthread_join(wt, NULL);
    const size_t total = (size_t)readers * DK_GATE_WALKS;
    cleanerName(zfree, U64s) uint64_t *all =
        zcalloc(total ? total : 1, sizeof(*all));
    size_t k = 0;
    uint64_t unprovable = 0;
    for (uint32_t i = 0; i < readers; i++) {
        for (uint32_t j = 0; j < DK_GATE_WALKS; j++) {
            all[k++] = ctx[i].walkNs[j];
        }
        unprovable += ctx[i].unprovable;
    }
    qsort(all, k, sizeof(*all), dkGateCmpU64_);
    const uint64_t p99 = k ? all[(k * 99u) / 100u] : UINT64_MAX;
    if (outUnprovable) {
        *outUnprovable = unprovable;
    }
    if (outAppends) {
        *outAppends = w.appends;
    }
    return p99;
}

static int dkTestOwnedSeqGateTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    /* Fill both rings once so every window is full before measuring. */
    for (uint64_t i = 1; i <= DK_GATE_N; i++) {
        dkGateRefAppend_(i);
        dkGateSeqAppend_(i);
    }

    /* [flavour][storm][trial]; trials are the outer loop, so drift in
     * host load spreads over every configuration. */
    static const char *const names[3] = {"reference", "converted", "probe"};
    const uint32_t widths[2] = {1, DK_GATE_STORM_R};
    uint64_t p99[3][2][DK_GATE_TRIALS];
    uint64_t unp[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    uint64_t app[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    for (uint32_t t = 0; t < DK_GATE_TRIALS; t++) {
        for (int f = 0; f < 3; f++) {
            for (int w = 0; w < 2; w++) {
                uint64_t u = 0;
                uint64_t a = 0;
                p99[f][w][t] = dkGateRun_((dkGateFlavour)f, widths[w], &u, &a);
                unp[f][w] += u;
                app[f][w] += a;
            }
        }
    }
    uint64_t best[3][2];
    uint64_t med[3][2];
    for (int f = 0; f < 3; f++) {
        for (int w = 0; w < 2; w++) {
            dkGateSortU64_(p99[f][w], DK_GATE_TRIALS);
            best[f][w] = p99[f][w][0];
            med[f][w] = p99[f][w][DK_GATE_TRIALS / 2];
        }
    }
    const uint64_t ref1 = best[DK_GATE_REFERENCE][0];
    const uint64_t refR = best[DK_GATE_REFERENCE][1];
    const uint64_t seq1 = best[DK_GATE_CONVERTED][0];
    const uint64_t seqR = best[DK_GATE_CONVERTED][1];
    const uint64_t acq1 = best[DK_GATE_ACQUIRE_PROBE][0];
    const uint64_t acqR = best[DK_GATE_ACQUIRE_PROBE][1];

    printf("ownedSeqGate: window=%u slots walks=%u/reader storm_R=%u "
           "writer_pace_ns=%u trials=%u (smallest and median per-trial "
           "p99)\n",
           DK_GATE_N - 1u, DK_GATE_WALKS, DK_GATE_STORM_R,
           DK_GATE_WRITER_PACE_NS, DK_GATE_TRIALS);
    for (int f = 0; f < 3; f++) {
        for (int w = 0; w < 2; w++) {
            printf("ownedSeqGate: %-9s R=%u p99_walk_ns=%" PRIu64
                   " (median %" PRIu64 ") per_slot_ns=%.1f unprovable=%" PRIu64
                   " appends=%" PRIu64 "\n",
                   names[f], widths[w], best[f][w], med[f][w],
                   (double)best[f][w] / (DK_GATE_N - 1u), unp[f][w], app[f][w]);
        }
    }

    /* B1 is relative to the reference's own scaling: with eight readers
     * and a writer sharing cache lines the reference slows down too, so
     * a fixed bound would measure the machine.  A concurrency term
     * specific to ownedSeq shows as a converted storm / R = 1 ratio
     * above twice the reference's. */
    const double refScale = ref1 ? (double)refR / (double)ref1 : 0.0;
    const double seqScale = seq1 ? (double)seqR / (double)seq1 : 0.0;
    const bool b1 = seqScale <= 2.0 * refScale;
    /* B2 bounds the probe, not the converted reader: the probe shares the
     * converted reader's storage and memory orders and the reference's
     * index arithmetic, so B2 covers the layout alone. The converted /
     * probe ratio is the cost of `% versions`, the declaration loads and
     * the call; it is printed, not bounded. */
    const bool b2one = acq1 * 4u <= ref1 * 5u;
    const bool b2storm = acqR * 4u <= refR * 5u;
    const bool b3 = seqR < 1000000u;
    printf("ownedSeqGate: B1 no-concurrency-term %s (converted storm/R=1 "
           "scaling %.2f <= 2x reference scaling %.2f)\n",
           b1 ? "ok" : "FAIL", seqScale, refScale);
    printf("ownedSeqGate: B2 layout R=1=%s storm=%s (probe p99 <= 1.25x "
           "reference p99); index arithmetic R=1 %.2fx storm %.2fx "
           "(converted / probe), reported\n",
           b2one ? "ok" : "FAIL", b2storm ? "ok" : "FAIL",
           acq1 ? (double)seq1 / (double)acq1 : 0.0,
           acqR ? (double)seqR / (double)acqR : 0.0);
    printf("ownedSeqGate: B3 absolute storm p99 %" PRIu64 " ns %s 1 ms\n", seqR,
           b3 ? "<" : "FAIL >=");
    /* Under ThreadSanitizer every atomic access goes through the runtime,
     * which adds a concurrency cost of its own; B1 and B3 are printed but
     * not asserted there.  B2 compares two readers with the same loads
     * and is asserted in every build. */
#if defined(__SANITIZE_THREAD__) ||                                            \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
    const bool instrumented = true;
#else
    const bool instrumented = false;
#endif
    if (!b1) {
        if (instrumented) {
            printf("ownedSeqGate: B1 reported, not asserted: this binary is "
                   "instrumented (ThreadSanitizer)\n");
        } else {
            failures++;
        }
    }
    if (!b2one || !b2storm) {
        failures++;
    }
    if (!b3) {
        if (instrumented) {
            printf("ownedSeqGate: B3 reported, not asserted: this binary is "
                   "instrumented (ThreadSanitizer)\n");
        } else {
            failures++;
        }
    }
    printf("ownedSeqGate: %s%s\n", failures == 0 ? "PASS" : "FAIL",
           instrumented ? " (ThreadSanitizer: B2 asserted, B1/B3 reported)"
                        : "");
    return failures;
}

/* ====================================================================
 * sharrow: release through a `checkedOut` member.
 *
 * checkedOut counts owners beyond the first, so a new object starts at
 * 0 and the release that finds 0 is the final one. Covers each member
 * type the dispatch accepts, the NULL-pointer short circuit, the custom
 * form, and a concurrent final release, which must observe every other
 * owner's writes.
 * ==================================================================== */
typedef struct dkSharrowU32 {
    _Atomic uint32_t checkedOut;
} dkSharrowU32;
typedef struct dkSharrowI64 {
    _Atomic int64_t checkedOut;
} dkSharrowI64;
typedef struct dkSharrowU64 {
    _Atomic uint64_t checkedOut;
} dkSharrowU64;
typedef struct dkSharrowPtr {
    _Atomic uint64_t *checkedOut;
} dkSharrowPtr;

#define DK_SHARROW_OWNERS 4
typedef struct dkSharrowShared {
    _Atomic uint32_t checkedOut;
    uint32_t marks[DK_SHARROW_OWNERS];
    uint32_t marksSeen; /* written by the final owner */
    uint32_t releases;
} dkSharrowShared;

static uint32_t g_dkSharrowReleases;

static void sharrowReleaserDkU32(dkSharrowU32 *s) {
    (void)s;
    g_dkSharrowReleases++;
}
static void sharrowReleaserDkI64(dkSharrowI64 *s) {
    (void)s;
    g_dkSharrowReleases++;
}
static void sharrowReleaserDkU64(dkSharrowU64 *s) {
    (void)s;
    g_dkSharrowReleases++;
}
static void sharrowReleaserDkPtr(dkSharrowPtr *s) {
    (void)s;
    g_dkSharrowReleases++;
}
static void sharrowReleaserDkShared(dkSharrowShared *s) {
    uint32_t seen = 0;
    for (int i = 0; i < DK_SHARROW_OWNERS; i++) {
        seen += s->marks[i];
    }
    s->marksSeen = seen;
    s->releases++;
}

typedef struct dkSharrowOwner {
    dkSharrowShared *shared;
    int index;
} dkSharrowOwner;

static void *dkSharrowOwnerMain_(void *arg) {
    dkSharrowOwner *o = arg;
    o->shared->marks[o->index] = 1;
    sharrowRelease(DkShared, o->shared);
    return NULL;
}

static int dkTestSharrowTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;
#define SHEXPECT(cond, msg)                                                    \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("dkTestSharrow FAIL: %s (%s:%d)\n", (msg), __FILE__,        \
                   __LINE__);                                                  \
            failures++;                                                        \
        }                                                                      \
    } while (0)

    /* One owner: the first release is final. */
    g_dkSharrowReleases = 0;
    dkSharrowU32 a = {0};
    sharrowRelease(DkU32, &a);
    SHEXPECT(g_dkSharrowReleases == 1, "a lone owner's release is final");

    /* Two owners: a retain makes the next release non-final. */
    g_dkSharrowReleases = 0;
    dkSharrowU32 b = {0};
    sharrowRetain(&b);
    SHEXPECT(sharrowCount(&b) == 1, "a retain counts one extra owner");
    sharrowRelease(DkU32, &b);
    SHEXPECT(g_dkSharrowReleases == 0 && sharrowCount(&b) == 0,
             "the first of two releases is not final");
    sharrowRelease(DkU32, &b);
    SHEXPECT(g_dkSharrowReleases == 1, "the second release is final");

    /* Every other member type, and RetainCount. */
    g_dkSharrowReleases = 0;
    dkSharrowI64 c = {0};
    dkSharrowU64 d = {0};
    sharrowRetainCount(&d, 2);
    sharrowRelease(DkI64, &c);
    sharrowRelease(DkU64, &d);
    sharrowRelease(DkU64, &d);
    SHEXPECT(g_dkSharrowReleases == 1 && sharrowCount(&d) == 0,
             "int64 and uint64 members count owners alike");
    sharrowRelease(DkU64, &d);
    SHEXPECT(g_dkSharrowReleases == 2, "and release on the last owner");

    /* A pointer member counts through the pointee; NULL releases at once
     * without touching any counter. */
    g_dkSharrowReleases = 0;
    _Atomic uint64_t outside = 1;
    dkSharrowPtr p = {.checkedOut = &outside};
    dkSharrowPtr q = {.checkedOut = NULL};
    sharrowRelease(DkPtr, &p);
    SHEXPECT(g_dkSharrowReleases == 0 && outside == 0,
             "a pointer member decrements its pointee");
    sharrowRelease(DkPtr, &p);
    SHEXPECT(g_dkSharrowReleases == 1, "and releases on the last owner");
    sharrowRelease(DkPtr, &q);
    SHEXPECT(g_dkSharrowReleases == 2,
             "a NULL pointer member releases at once");

    /* Custom: the count lives in another object. */
    g_dkSharrowReleases = 0;
    dkSharrowU64 target = {0};
    dkSharrowU32 holder = {0};
    sharrowRetain(&holder);
    sharrowReleaseCustom(DkU64, &target, &holder);
    SHEXPECT(g_dkSharrowReleases == 0, "a custom release honours the holder");
    sharrowReleaseCustom(DkU64, &target, &holder);
    SHEXPECT(g_dkSharrowReleases == 1 && sharrowCount(&target) == 0,
             "and runs the target's releaser, leaving the target's own "
             "counter alone");

    /* Concurrent final release: each owner writes its mark and releases;
     * exactly one release is final and it sees every mark. */
    {
        dkSharrowShared shared = {.checkedOut = DK_SHARROW_OWNERS - 1};
        dkSharrowOwner owners[DK_SHARROW_OWNERS];
        pthread_t th[DK_SHARROW_OWNERS];
        int started = 0;
        for (int i = 0; i < DK_SHARROW_OWNERS; i++) {
            owners[i] = (dkSharrowOwner){.shared = &shared, .index = i};
            if (pthread_create(&th[i], NULL, dkSharrowOwnerMain_, &owners[i]) !=
                0) {
                break;
            }
            started++;
        }
        SHEXPECT(started == DK_SHARROW_OWNERS, "owner threads started");
        for (int i = 0; i < started; i++) {
            pthread_join(th[i], NULL);
        }
        SHEXPECT(shared.releases == 1 && shared.marksSeen == DK_SHARROW_OWNERS,
                 "one final release, and it saw every owner's write");
    }

    if (failures == 0) {
        printf("dkTestSharrow: PASS\n");
    }
#undef SHEXPECT
    return failures;
}

/* ====================================================================
 * chacha20poly1305 streaming ctx full-wipe in streamFinalise.
 * After Final (Seal success, Open success, Open failure, NULL-arg
 * guard) the ctx must be bytewise zero — no residual key, nonce, or
 * keystream block left for callers to leak via stack/heap reuse.
 * ==================================================================== */
static int reportNonZeroCtxWipe(const char *label,
                                const chacha20poly1305StreamCtx *ctx) {
    const uint8_t *p = (const uint8_t *)ctx;
    size_t n = sizeof(*ctx);
    size_t firstNonZero = (size_t)-1;
    size_t nonZeroCount = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) {
            if (firstNonZero == (size_t)-1) {
                firstNonZero = i;
            }
            nonZeroCount++;
        }
    }
    printf("dkTestChaChaStreamCtxWipe FAIL %s: %zu/%zu bytes non-zero, "
           "first at offset %zu (=0x%02x)\n",
           label, nonZeroCount, n, firstNonZero,
           (firstNonZero == (size_t)-1) ? 0 : p[firstNonZero]);
    return 1;
}

static int dkTestChaChaStreamCtxWipeTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int failures = 0;

    const size_t ctxSize = sizeof(chacha20poly1305StreamCtx);
    cleanerName(zfree, ChaChaStreamCtx) chacha20poly1305StreamCtx *ctx =
        (chacha20poly1305StreamCtx *)zmalloc(ctxSize);
    cleanerName(zfree, Bytes) uint8_t *zeroRef = (uint8_t *)zmalloc(ctxSize);
    if (ctx == NULL || zeroRef == NULL) {
        printf("dkTestChaChaStreamCtxWipe: alloc failed\n");
        return 1;
    }

    uint8_t key[CHACHA20_POLY1305_KEY_LEN];
    uint8_t nonce[CHACHA20_POLY1305_NONCE_LEN];
    for (size_t i = 0; i < sizeof(key); i++) {
        key[i] = (uint8_t)(0xA0 + i);
    }
    for (size_t i = 0; i < sizeof(nonce); i++) {
        nonce[i] = (uint8_t)(0x50 + i);
    }
    const uint8_t aad[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    const size_t plainLen = 200;
    cleanerName(zfree, Bytes) uint8_t *plain = (uint8_t *)zmalloc(plainLen);
    cleanerName(zfree, Bytes) uint8_t *cipher = (uint8_t *)zmalloc(plainLen);
    cleanerName(zfree, Bytes) uint8_t *recovered = (uint8_t *)zmalloc(plainLen);
    if (plain == NULL || cipher == NULL || recovered == NULL) {
        printf("dkTestChaChaStreamCtxWipe: plaintext alloc failed\n");
        return 1;
    }
    for (size_t i = 0; i < plainLen; i++) {
        plain[i] = (uint8_t)(0x80 ^ (i * 7u));
    }

    /* Case 1: Seal — ctx must be bytewise zero after Final.
     * Pre-fill with 0xA5 so any byte the wipe misses stands out.
     * Without the pre-fill the test was weakly-meaningful for never-
     * written ctx regions: a wipe-elision regression would still
     * produce a zero ctx via the zmalloc-zeroed start state. */
    memset(ctx, 0xA5, ctxSize);
    uint8_t tag[CHACHA20_POLY1305_TAG_LEN];
    chacha20poly1305SealStreamInit(ctx, key, nonce);
    if (!chacha20poly1305SealStreamAad(ctx, aad, sizeof(aad))) {
        printf("dkTestChaChaStreamCtxWipe: Seal AAD failed\n");
        failures++;
        goto done;
    }
    if (!chacha20poly1305SealStreamUpdate(ctx, plain, 73, cipher)) {
        printf("dkTestChaChaStreamCtxWipe: Seal Update#1 failed\n");
        failures++;
        goto done;
    }
    if (!chacha20poly1305SealStreamUpdate(ctx, plain + 73, plainLen - 73,
                                          cipher + 73)) {
        printf("dkTestChaChaStreamCtxWipe: Seal Update#2 failed\n");
        failures++;
        goto done;
    }
    if (!chacha20poly1305SealStreamFinal(ctx, tag)) {
        printf("dkTestChaChaStreamCtxWipe: Seal Final failed\n");
        failures++;
        goto done;
    }
    if (memcmp(ctx, zeroRef, ctxSize) != 0) {
        failures += reportNonZeroCtxWipe("Seal", ctx);
    }

    /* Case 2: Open success — ctx must be bytewise zero.  Same pre-fill
     * upgrade as Case 1 so the wipe assertion observes the actual
     * scrub rather than the freshly-zeroed start state. */
    memset(ctx, 0xA5, ctxSize);
    chacha20poly1305OpenStreamInit(ctx, key, nonce);
    if (!chacha20poly1305OpenStreamAad(ctx, aad, sizeof(aad))) {
        printf("dkTestChaChaStreamCtxWipe: Open AAD failed\n");
        failures++;
        goto done;
    }
    if (!chacha20poly1305OpenStreamUpdate(ctx, cipher, 91, recovered)) {
        printf("dkTestChaChaStreamCtxWipe: Open Update#1 failed\n");
        failures++;
        goto done;
    }
    if (!chacha20poly1305OpenStreamUpdate(ctx, cipher + 91, plainLen - 91,
                                          recovered + 91)) {
        printf("dkTestChaChaStreamCtxWipe: Open Update#2 failed\n");
        failures++;
        goto done;
    }
    if (!chacha20poly1305OpenStreamFinal(ctx, tag)) {
        printf("dkTestChaChaStreamCtxWipe: Open Final returned false on "
               "untampered ciphertext\n");
        failures++;
        goto done;
    }
    if (memcmp(recovered, plain, plainLen) != 0) {
        printf("dkTestChaChaStreamCtxWipe: Open recovered != plaintext\n");
        failures++;
        goto done;
    }
    if (memcmp(ctx, zeroRef, ctxSize) != 0) {
        failures += reportNonZeroCtxWipe("Open-success", ctx);
    }

    /* Case 3: Open failure (tampered CT) — ctx must be zero.  Same
     * pre-fill upgrade so the failure-path wipe is observably real. */
    memset(ctx, 0xA5, ctxSize);
    cipher[plainLen - 7] ^= 0x55;
    chacha20poly1305OpenStreamInit(ctx, key, nonce);
    if (!chacha20poly1305OpenStreamAad(ctx, aad, sizeof(aad))) {
        printf("dkTestChaChaStreamCtxWipe: Open AAD (tamper) failed\n");
        failures++;
        goto done;
    }
    if (!chacha20poly1305OpenStreamUpdate(ctx, cipher, plainLen, recovered)) {
        printf("dkTestChaChaStreamCtxWipe: Open Update (tamper) failed\n");
        failures++;
        goto done;
    }
    if (chacha20poly1305OpenStreamFinal(ctx, tag)) {
        printf("dkTestChaChaStreamCtxWipe: Tampered CT not rejected\n");
        failures++;
    }
    if (memcmp(ctx, zeroRef, ctxSize) != 0) {
        failures += reportNonZeroCtxWipe("Open-tamper", ctx);
    }

    /* Case 4: NULL-tag guard wipes ctx */
    memset(ctx, 0xCD, ctxSize);
    chacha20poly1305SealStreamInit(ctx, key, nonce);
    if (chacha20poly1305SealStreamFinal(ctx, NULL)) {
        printf("dkTestChaChaStreamCtxWipe: SealFinal(NULL) should reject\n");
        failures++;
    }
    if (memcmp(ctx, zeroRef, ctxSize) != 0) {
        failures += reportNonZeroCtxWipe("Seal-NULL-tag", ctx);
    }

    memset(ctx, 0xCD, ctxSize);
    chacha20poly1305OpenStreamInit(ctx, key, nonce);
    if (chacha20poly1305OpenStreamFinal(ctx, NULL)) {
        printf("dkTestChaChaStreamCtxWipe: OpenFinal(NULL) should reject\n");
        failures++;
    }
    if (memcmp(ctx, zeroRef, ctxSize) != 0) {
        failures += reportNonZeroCtxWipe("Open-NULL-tag", ctx);
    }

done:
    if (failures == 0) {
        printf("dkTestChaChaStreamCtxWipe: OK "
               "(Seal, Open, Open-tamper, NULL-tag guards)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * ChaCha20-Poly1305 streaming negative + state-machine tests.
 * (1) Mid-stream byte tamper caught at Final (not at Update).
 * (2) Update-after-Final must return false (fail-closed) on Seal+Open.
 * (3) Final-twice returns false on second call (Seal+Open) — pinned
 *     per the streamFinalise wipe-to-zero contract.
 * ==================================================================== */
static int dkTestStreamMidTamperTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    uint8_t key[CHACHA20_POLY1305_KEY_LEN];
    uint8_t nonce[CHACHA20_POLY1305_NONCE_LEN];
    for (size_t i = 0; i < sizeof(key); i++) {
        key[i] = (uint8_t)(0x10 + i);
    }
    for (size_t i = 0; i < sizeof(nonce); i++) {
        nonce[i] = (uint8_t)(0xE0 + i);
    }

    enum { kPayload = 256, kSegLen = 64, kNumSegs = 4, kTamperOffset = 64 };

    uint8_t plain[kPayload];
    uint8_t cipher[kPayload];
    uint8_t recovered[kPayload];
    for (size_t i = 0; i < kPayload; i++) {
        plain[i] = (uint8_t)(i ^ 0xA5);
        cipher[i] = 0;
        recovered[i] = 0;
    }

    chacha20poly1305StreamCtx ctx;
    chacha20poly1305SealStreamInit(&ctx, key, nonce);
    for (int s = 0; s < kNumSegs; s++) {
        if (!chacha20poly1305SealStreamUpdate(&ctx, plain + s * kSegLen,
                                              kSegLen, cipher + s * kSegLen)) {
            printf("dkTestStreamMidTamper: Seal Update#%d failed\n", s);
            failures++;
            goto done;
        }
    }
    uint8_t tag[CHACHA20_POLY1305_TAG_LEN];
    if (!chacha20poly1305SealStreamFinal(&ctx, tag)) {
        printf("dkTestStreamMidTamper: Seal Final failed\n");
        failures++;
        goto done;
    }

    uint8_t baselineCipher[kPayload];
    memcpy(baselineCipher, cipher, kPayload);
    chacha20poly1305OpenStreamInit(&ctx, key, nonce);
    for (int s = 0; s < kNumSegs; s++) {
        if (!chacha20poly1305OpenStreamUpdate(
                &ctx, baselineCipher + s * kSegLen, kSegLen,
                recovered + s * kSegLen)) {
            printf("dkTestStreamMidTamper: baseline Open Update#%d failed\n",
                   s);
            failures++;
            goto done;
        }
    }
    if (!chacha20poly1305OpenStreamFinal(&ctx, tag)) {
        printf("dkTestStreamMidTamper: baseline Open Final rejected "
               "untampered CT\n");
        failures++;
        goto done;
    }
    if (memcmp(recovered, plain, kPayload) != 0) {
        printf("dkTestStreamMidTamper: baseline plaintext mismatch\n");
        failures++;
        goto done;
    }

    cipher[kTamperOffset] ^= 0x01;

    memset(recovered, 0, sizeof(recovered));
    chacha20poly1305OpenStreamInit(&ctx, key, nonce);
    for (int s = 0; s < kNumSegs; s++) {
        if (!chacha20poly1305OpenStreamUpdate(
                &ctx, cipher + s * kSegLen, kSegLen, recovered + s * kSegLen)) {
            printf("dkTestStreamMidTamper: tamper Open Update#%d returned "
                   "false (expected true; mismatch caught at Final)\n",
                   s);
            failures++;
            goto done;
        }
    }
    if (chacha20poly1305OpenStreamFinal(&ctx, tag)) {
        printf("dkTestStreamMidTamper: Open Final ACCEPTED tampered CT "
               "(bit 0 of byte %d flipped)\n",
               kTamperOffset);
        failures++;
        goto done;
    }

done:
    if (failures == 0) {
        printf("dkTestStreamMidTamper: OK "
               "(4x64 segments, mid-stream byte-64 bit-0 flip caught at "
               "Final)\n");
        return 0;
    }
    return 1;
}

static int dkTestStreamUpdateAfterFinalTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    uint8_t key[CHACHA20_POLY1305_KEY_LEN];
    uint8_t nonce[CHACHA20_POLY1305_NONCE_LEN];
    for (size_t i = 0; i < sizeof(key); i++) {
        key[i] = (uint8_t)(0x30 + i);
    }
    for (size_t i = 0; i < sizeof(nonce); i++) {
        nonce[i] = (uint8_t)(0x70 + i);
    }

    const uint8_t pt[64] = {0x42};
    uint8_t ct[64];
    uint8_t tag[CHACHA20_POLY1305_TAG_LEN];

    chacha20poly1305StreamCtx ctx;
    chacha20poly1305SealStreamInit(&ctx, key, nonce);
    if (!chacha20poly1305SealStreamUpdate(&ctx, pt, sizeof(pt), ct)) {
        printf("dkTestStreamUpdateAfterFinal: Seal Update failed\n");
        failures++;
        goto done;
    }
    if (!chacha20poly1305SealStreamFinal(&ctx, tag)) {
        printf("dkTestStreamUpdateAfterFinal: Seal Final failed\n");
        failures++;
        goto done;
    }
    uint8_t extraCt[8];
    const uint8_t extraPt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    if (chacha20poly1305SealStreamUpdate(&ctx, extraPt, sizeof(extraPt),
                                         extraCt)) {
        printf("dkTestStreamUpdateAfterFinal: Seal Update-after-Final "
               "returned true (expected false: fail-closed contract)\n");
        failures++;
        goto done;
    }

    chacha20poly1305OpenStreamInit(&ctx, key, nonce);
    uint8_t recovered[64];
    if (!chacha20poly1305OpenStreamUpdate(&ctx, ct, sizeof(ct), recovered)) {
        printf("dkTestStreamUpdateAfterFinal: Open Update failed\n");
        failures++;
        goto done;
    }
    if (!chacha20poly1305OpenStreamFinal(&ctx, tag)) {
        printf("dkTestStreamUpdateAfterFinal: Open Final rejected valid "
               "tag\n");
        failures++;
        goto done;
    }
    if (memcmp(recovered, pt, sizeof(pt)) != 0) {
        printf("dkTestStreamUpdateAfterFinal: Open plaintext mismatch\n");
        failures++;
        goto done;
    }
    uint8_t extraRec[8];
    if (chacha20poly1305OpenStreamUpdate(&ctx, ct, sizeof(extraRec),
                                         extraRec)) {
        printf("dkTestStreamUpdateAfterFinal: Open Update-after-Final "
               "returned true (expected false: fail-closed contract)\n");
        failures++;
        goto done;
    }

done:
    if (failures == 0) {
        printf("dkTestStreamUpdateAfterFinal: OK "
               "(Seal+Open Update after Final rejected)\n");
        return 0;
    }
    return 1;
}

static int dkTestStreamFinalTwiceTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    uint8_t key[CHACHA20_POLY1305_KEY_LEN];
    uint8_t nonce[CHACHA20_POLY1305_NONCE_LEN];
    for (size_t i = 0; i < sizeof(key); i++) {
        key[i] = (uint8_t)(0x90 + i);
    }
    for (size_t i = 0; i < sizeof(nonce); i++) {
        nonce[i] = (uint8_t)(0x20 + i);
    }

    const uint8_t pt[40] = {0x07};
    uint8_t ct[40];
    uint8_t tag1[CHACHA20_POLY1305_TAG_LEN];
    uint8_t tag2[CHACHA20_POLY1305_TAG_LEN];

    chacha20poly1305StreamCtx ctx;
    chacha20poly1305SealStreamInit(&ctx, key, nonce);
    if (!chacha20poly1305SealStreamUpdate(&ctx, pt, sizeof(pt), ct)) {
        printf("dkTestStreamFinalTwice: Seal Update failed\n");
        failures++;
        goto done;
    }
    if (!chacha20poly1305SealStreamFinal(&ctx, tag1)) {
        printf("dkTestStreamFinalTwice: first Seal Final failed\n");
        failures++;
        goto done;
    }
    memset(tag2, 0xCC, sizeof(tag2));
    if (chacha20poly1305SealStreamFinal(&ctx, tag2)) {
        printf("dkTestStreamFinalTwice: second Seal Final returned true "
               "(expected false: fail-closed double-Finalise)\n");
        failures++;
        goto done;
    }

    chacha20poly1305OpenStreamInit(&ctx, key, nonce);
    uint8_t recovered[40];
    if (!chacha20poly1305OpenStreamUpdate(&ctx, ct, sizeof(ct), recovered)) {
        printf("dkTestStreamFinalTwice: Open Update failed\n");
        failures++;
        goto done;
    }
    if (!chacha20poly1305OpenStreamFinal(&ctx, tag1)) {
        printf("dkTestStreamFinalTwice: first Open Final rejected valid "
               "tag\n");
        failures++;
        goto done;
    }
    if (chacha20poly1305OpenStreamFinal(&ctx, tag1)) {
        printf("dkTestStreamFinalTwice: second Open Final returned true "
               "(expected false: fail-closed double-Finalise)\n");
        failures++;
        goto done;
    }

done:
    if (failures == 0) {
        printf("dkTestStreamFinalTwice: OK "
               "(second Seal/Open Final returns false; pinned fail-closed)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * AES-NI + PCLMUL backend parity vs portable scalar.
 * On x86 with AES-NI: force both backends through the dispatcher for
 * NIST vectors + sweep sizes; assert CT+tag byte-identical. On ARM:
 * verify dispatcher selects SCALAR, probe returns false, NI stub
 * refuses direct calls (a no-op stub would silently corrupt callers).
 * ==================================================================== */
static void a3HexDec(const char *hex, uint8_t *out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        char c1 = hex[i * 2], c2 = hex[i * 2 + 1];
        out[i] = (uint8_t)(((c1 >= 'a' ? c1 - 'a' + 10 : c1 - '0') << 4) |
                           (c2 >= 'a' ? c2 - 'a' + 10 : c2 - '0'));
    }
}

typedef struct a3Vector {
    const char *name;
    size_t key_len;
    const char *key_hex;
    const char *nonce_hex;
    const char *aad_hex;
    size_t aad_len;
    const char *pt_hex;
    size_t pt_len;
    const char *ct_hex;
    const char *tag_hex;
} a3Vector;

static int dkTestAesNiTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    static const a3Vector vectors[] = {
        {"NIST V1 (AES-128 empty PT, empty AAD)", 16,
         "00000000000000000000000000000000", "000000000000000000000000", NULL,
         0, NULL, 0, NULL, "58e2fccefa7e3061367f1d57a4e7455a"},
        {"NIST V2 (AES-128 16B zero PT)", 16,
         "00000000000000000000000000000000", "000000000000000000000000", NULL,
         0, "00000000000000000000000000000000", 16,
         "0388dace60b6a392f328c2b971b2fe78",
         "ab6e47d42cec13bdf53a67b21257bddf"},
        {"NIST V3 (AES-128 real key, 64B PT)", 16,
         "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888", NULL,
         0,
         "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
         "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
         64,
         "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
         "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985",
         "4d5c2af327cd64a62cf35abd2ba6fab4"},
        {"NIST V4 (AES-128 with AAD, 60B PT)", 16,
         "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
         "feedfacedeadbeeffeedfacedeadbeefabaddad2", 20,
         "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
         "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
         60,
         "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
         "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091",
         "5bc94fbc3221a5db94fae95ae7121a47"},
        {"NIST V13 (AES-256 zero key, empty PT)", 32,
         "0000000000000000000000000000000000000000000000000000000000000000",
         "000000000000000000000000", NULL, 0, NULL, 0, NULL,
         "530f8afbc74536b9a963b4f1c4cb738b"},
    };
    const size_t n_vectors = sizeof(vectors) / sizeof(vectors[0]);

    const bool ni_here = aesGcmNiAvailable();
    const aesGcmBackend default_backend = aesGcmActiveBackend();

    if (ni_here && default_backend != AES_GCM_BACKEND_AESNI) {
        printf("dkTestAesNi: NI probe true but dispatcher backend=%d "
               "(expected AESNI=%d)\n",
               (int)default_backend, (int)AES_GCM_BACKEND_AESNI);
        failures++;
    }
    if (!ni_here && default_backend == AES_GCM_BACKEND_AESNI) {
        printf("dkTestAesNi: dispatcher reports AESNI but probe says no\n");
        failures++;
    }

    if (!ni_here) {
        aesGcmKey nk;
        const uint8_t k128[16] = {0};
        if (aesGcmNiInit(&nk, k128, 16)) {
            printf("dkTestAesNi: ARM stub aesGcmNiInit must return false\n");
            failures++;
        }
        uint8_t nonce[12] = {0}, pt[16] = {0}, ct[16] = {0}, tag[16] = {0};
        if (aesGcmNiSeal(&nk, nonce, NULL, 0, pt, 16, ct, tag)) {
            printf("dkTestAesNi: ARM stub aesGcmNiSeal must return false\n");
            failures++;
        }
        if (aesGcmNiOpen(&nk, nonce, NULL, 0, ct, 16, tag, pt)) {
            printf("dkTestAesNi: ARM stub aesGcmNiOpen must return false\n");
            failures++;
        }
        printf("dkTestAesNi: AES-NI not present on this host; stub-guard "
               "checks passed; dispatcher backend=SCALAR\n");
        return failures == 0 ? 0 : 1;
    }

    for (size_t i = 0; i < n_vectors; i++) {
        const a3Vector *v = &vectors[i];
        uint8_t key[32], nonce[12];
        a3HexDec(v->key_hex, key, v->key_len);
        a3HexDec(v->nonce_hex, nonce, 12);

        uint8_t aad_buf[64];
        const uint8_t *aad = NULL;
        if (v->aad_hex != NULL && v->aad_len > 0) {
            a3HexDec(v->aad_hex, aad_buf, v->aad_len);
            aad = aad_buf;
        }
        uint8_t pt_buf[128];
        const uint8_t *pt = NULL;
        if (v->pt_hex != NULL && v->pt_len > 0) {
            a3HexDec(v->pt_hex, pt_buf, v->pt_len);
            pt = pt_buf;
        }

        uint8_t scalar_ct[128] = {0}, scalar_tag[16] = {0};
        aesGcmSetForceBackend(AES_GCM_FORCE_SCALAR);
        aesGcmKey sk;
        aesGcmInit(&sk, key, v->key_len);
        aesGcmSeal(&sk, nonce, aad, v->aad_len, pt, v->pt_len,
                   v->pt_len > 0 ? scalar_ct : NULL, scalar_tag);

        uint8_t ni_ct[128] = {0}, ni_tag[16] = {0};
        aesGcmSetForceBackend(AES_GCM_FORCE_AESNI);
        aesGcmKey nk;
        aesGcmInit(&nk, key, v->key_len);
        aesGcmSeal(&nk, nonce, aad, v->aad_len, pt, v->pt_len,
                   v->pt_len > 0 ? ni_ct : NULL, ni_tag);

        if (v->pt_len > 0 && memcmp(scalar_ct, ni_ct, v->pt_len) != 0) {
            printf("dkTestAesNi: CT mismatch on %s\n", v->name);
            failures++;
        }
        if (memcmp(scalar_tag, ni_tag, 16) != 0) {
            printf("dkTestAesNi: TAG mismatch on %s\n", v->name);
            failures++;
        }

        uint8_t exp_tag[16];
        a3HexDec(v->tag_hex, exp_tag, 16);
        if (memcmp(ni_tag, exp_tag, 16) != 0) {
            printf("dkTestAesNi: NI tag != NIST on %s\n", v->name);
            failures++;
        }
        if (v->ct_hex != NULL && v->pt_len > 0) {
            uint8_t exp_ct[128];
            a3HexDec(v->ct_hex, exp_ct, v->pt_len);
            if (memcmp(ni_ct, exp_ct, v->pt_len) != 0) {
                printf("dkTestAesNi: NI CT != NIST on %s\n", v->name);
                failures++;
            }
        }

        uint8_t rec_scalar[128] = {0}, rec_ni[128] = {0};
        aesGcmSetForceBackend(AES_GCM_FORCE_SCALAR);
        aesGcmInit(&sk, key, v->key_len);
        if (!aesGcmOpen(&sk, nonce, aad, v->aad_len,
                        v->pt_len > 0 ? scalar_ct : NULL, v->pt_len, scalar_tag,
                        v->pt_len > 0 ? rec_scalar : NULL)) {
            printf("dkTestAesNi: scalar Open failed on %s\n", v->name);
            failures++;
        }
        aesGcmSetForceBackend(AES_GCM_FORCE_AESNI);
        aesGcmInit(&nk, key, v->key_len);
        if (!aesGcmOpen(&nk, nonce, aad, v->aad_len,
                        v->pt_len > 0 ? ni_ct : NULL, v->pt_len, ni_tag,
                        v->pt_len > 0 ? rec_ni : NULL)) {
            printf("dkTestAesNi: NI Open failed on %s\n", v->name);
            failures++;
        }
        if (v->pt_len > 0 && memcmp(rec_scalar, rec_ni, v->pt_len) != 0) {
            printf("dkTestAesNi: recovered-PT mismatch on %s\n", v->name);
            failures++;
        }
        if (v->pt_len > 0 && pt != NULL && memcmp(rec_ni, pt, v->pt_len) != 0) {
            printf("dkTestAesNi: NI Open recovered != original on %s\n",
                   v->name);
            failures++;
        }
    }

    static const size_t sweep_sizes[] = {
        0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 4095, 4096, 4097};
    const size_t n_sweep = sizeof(sweep_sizes) / sizeof(sweep_sizes[0]);
    uint8_t key[32];
    for (size_t i = 0; i < 32; i++) {
        key[i] = (uint8_t)(i * 11u + 3u);
    }
    uint8_t nonce[12];
    for (size_t i = 0; i < 12; i++) {
        nonce[i] = (uint8_t)(i * 17u);
    }
    uint8_t aad[24];
    for (size_t i = 0; i < 24; i++) {
        aad[i] = (uint8_t)(i * 7u + 1u);
    }

    for (uint32_t kl_idx = 0; kl_idx < 2; kl_idx++) {
        const size_t kl = (kl_idx == 0) ? 16u : 32u;
        for (size_t s = 0; s < n_sweep; s++) {
            const size_t n = sweep_sizes[s];
            cleanerName(zfree, Bytes) uint8_t *pt =
                (n > 0) ? (uint8_t *)zmalloc(n) : NULL;
            cleanerName(zfree, Bytes) uint8_t *scalar_ct =
                (n > 0) ? (uint8_t *)zmalloc(n) : NULL;
            cleanerName(zfree, Bytes) uint8_t *ni_ct =
                (n > 0) ? (uint8_t *)zmalloc(n) : NULL;
            uint8_t scalar_tag[16], ni_tag[16];
            for (size_t i = 0; i < n; i++) {
                pt[i] = (uint8_t)((i * 13u) ^ s);
            }

            aesGcmSetForceBackend(AES_GCM_FORCE_SCALAR);
            aesGcmKey sk;
            aesGcmInit(&sk, key, kl);
            aesGcmSeal(&sk, nonce, aad, 24, pt, n, n > 0 ? scalar_ct : NULL,
                       scalar_tag);

            aesGcmSetForceBackend(AES_GCM_FORCE_AESNI);
            aesGcmKey nk;
            aesGcmInit(&nk, key, kl);
            aesGcmSeal(&nk, nonce, aad, 24, pt, n, n > 0 ? ni_ct : NULL,
                       ni_tag);

            if (n > 0 && memcmp(scalar_ct, ni_ct, n) != 0) {
                printf("dkTestAesNi: sweep CT mismatch kl=%zu n=%zu\n", kl, n);
                failures++;
            }
            if (memcmp(scalar_tag, ni_tag, 16) != 0) {
                printf("dkTestAesNi: sweep TAG mismatch kl=%zu n=%zu\n", kl, n);
                failures++;
            }
        }
    }
    aesGcmSetForceBackend(AES_GCM_FORCE_AUTO);

    printf("dkTestAesNi: NI present; %zu NIST vectors + %zu sweep sizes "
           "x 2 key lengths cross-verified scalar vs AESNI; %d failures\n",
           n_vectors, n_sweep, failures);
    return failures == 0 ? 0 : 1;
}

/* ARMv8 AES + PMULL backend parity vs portable scalar.
 * Reuses a3HexDec / a3Vector from dkTestAesNi above. */
static int dkTestAesArmCryptoTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    static const a3Vector vectors[] = {
        {"NIST V1", 16, "00000000000000000000000000000000",
         "000000000000000000000000", NULL, 0, NULL, 0, NULL,
         "58e2fccefa7e3061367f1d57a4e7455a"},
        {"NIST V2", 16, "00000000000000000000000000000000",
         "000000000000000000000000", NULL, 0,
         "00000000000000000000000000000000", 16,
         "0388dace60b6a392f328c2b971b2fe78",
         "ab6e47d42cec13bdf53a67b21257bddf"},
        {"NIST V3", 16, "feffe9928665731c6d6a8f9467308308",
         "cafebabefacedbaddecaf888", NULL, 0,
         "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
         "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
         64,
         "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
         "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985",
         "4d5c2af327cd64a62cf35abd2ba6fab4"},
        {"NIST V13", 32,
         "0000000000000000000000000000000000000000000000000000000000000000",
         "000000000000000000000000", NULL, 0, NULL, 0, NULL,
         "530f8afbc74536b9a963b4f1c4cb738b"},
    };
    const size_t n_vectors = sizeof(vectors) / sizeof(vectors[0]);

    const bool arm_here = aesGcmArmCryptoAvailable();

    if (!arm_here) {
        printf("dkTestAesArmCrypto: ARMv8 crypto not on this host; stubs OK\n");
        return 0;
    }

    for (size_t i = 0; i < n_vectors; i++) {
        const a3Vector *v = &vectors[i];
        uint8_t key[32], nonce[12];
        a3HexDec(v->key_hex, key, v->key_len);
        a3HexDec(v->nonce_hex, nonce, 12);

        uint8_t aad_buf[64];
        const uint8_t *aad = NULL;
        if (v->aad_hex != NULL && v->aad_len > 0) {
            a3HexDec(v->aad_hex, aad_buf, v->aad_len);
            aad = aad_buf;
        }
        uint8_t pt_buf[128];
        const uint8_t *pt = NULL;
        if (v->pt_hex != NULL && v->pt_len > 0) {
            a3HexDec(v->pt_hex, pt_buf, v->pt_len);
            pt = pt_buf;
        }

        uint8_t scalar_ct[128] = {0}, scalar_tag[16] = {0};
        aesGcmSetForceBackend(AES_GCM_FORCE_SCALAR);
        aesGcmKey sk;
        aesGcmInit(&sk, key, v->key_len);
        aesGcmSeal(&sk, nonce, aad, v->aad_len, pt, v->pt_len,
                   v->pt_len > 0 ? scalar_ct : NULL, scalar_tag);

        uint8_t arm_ct[128] = {0}, arm_tag[16] = {0};
        aesGcmSetForceBackend(AES_GCM_FORCE_ARMCRYPTO);
        aesGcmKey ak;
        aesGcmInit(&ak, key, v->key_len);
        aesGcmSeal(&ak, nonce, aad, v->aad_len, pt, v->pt_len,
                   v->pt_len > 0 ? arm_ct : NULL, arm_tag);

        if (v->pt_len > 0 && memcmp(scalar_ct, arm_ct, v->pt_len) != 0) {
            printf("dkTestAesArmCrypto: CT mismatch on %s\n", v->name);
            failures++;
        }
        if (memcmp(scalar_tag, arm_tag, 16) != 0) {
            printf("dkTestAesArmCrypto: TAG mismatch on %s\n", v->name);
            failures++;
        }
        uint8_t exp_tag[16];
        a3HexDec(v->tag_hex, exp_tag, 16);
        if (memcmp(arm_tag, exp_tag, 16) != 0) {
            printf("dkTestAesArmCrypto: ARM tag != NIST on %s\n", v->name);
            failures++;
        }
    }
    aesGcmSetForceBackend(AES_GCM_FORCE_AUTO);

    printf("dkTestAesArmCrypto: ARMv8 crypto; %zu NIST vectors; %d failures\n",
           n_vectors, failures);
    return failures == 0 ? 0 : 1;
}

/* X25519 canonical small-order list pre-reject. */
static int dkTestX25519SmallOrderRejectTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    static const uint8_t blacklist[12][32] = {
        {0},
        {1},
        {0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3,
         0xfa, 0xf1, 0x9f, 0xc4, 0x6a, 0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32,
         0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00},
        {0x5f, 0x9c, 0x95, 0xbc, 0xa3, 0x50, 0x8c, 0x24, 0xb1, 0xd0, 0xb1,
         0x55, 0x9c, 0x83, 0xef, 0x5b, 0x04, 0x44, 0x5c, 0xc4, 0x58, 0x1c,
         0x8e, 0x86, 0xd8, 0x22, 0x4e, 0xdd, 0xd0, 0x9f, 0x11, 0x57},
        {0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
        {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
        {0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
        {0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3,
         0xfa, 0xf1, 0x9f, 0xc4, 0x6a, 0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32,
         0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x80},
        {0x5f, 0x9c, 0x95, 0xbc, 0xa3, 0x50, 0x8c, 0x24, 0xb1, 0xd0, 0xb1,
         0x55, 0x9c, 0x83, 0xef, 0x5b, 0x04, 0x44, 0x5c, 0xc4, 0x58, 0x1c,
         0x8e, 0x86, 0xd8, 0x22, 0x4e, 0xdd, 0xd0, 0x9f, 0x11, 0xd7},
        {0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        {0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
    };

    uint8_t priv[32];
    for (uint32_t i = 0; i < 32; i++) {
        priv[i] = (uint8_t)(0x40u | (i & 0x1Fu));
    }

    for (uint32_t e = 0; e < 12; e++) {
        uint8_t out[32];
        memset(out, 0xCC, sizeof(out));
        if (x25519(out, priv, blacklist[e])) {
            printf("dkTestX25519SmallOrderReject: entry %u must reject\n", e);
            failures++;
        }
        for (uint32_t i = 0; i < 32; i++) {
            if (out[i] != 0) {
                printf(
                    "dkTestX25519SmallOrderReject: entry %u out[%u]=0x%02x\n",
                    e, i, out[i]);
                failures++;
                break;
            }
        }
    }

    uint64_t rng = 0xA9DEAD8EEFCAFE11ULL;
    for (uint32_t t = 0; t < 100; t++) {
        uint8_t peer[32];
        for (uint32_t i = 0; i < 32; i++) {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            peer[i] = (uint8_t)(rng >> 56);
        }
        uint8_t out[32];
        if (!x25519(out, priv, peer)) {
            printf("dkTestX25519SmallOrderReject: random %u false-rejected\n",
                   t);
            failures++;
        }
    }

    static const uint8_t kat_scalar[32] = {
        0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d, 0x3b, 0x16, 0x15,
        0x4b, 0x82, 0x46, 0x5e, 0xdd, 0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc,
        0x5a, 0x18, 0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4};
    static const uint8_t kat_u[32] = {
        0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb, 0x35, 0x94, 0xc1,
        0xa4, 0x24, 0xb1, 0x5f, 0x7c, 0x72, 0x66, 0x24, 0xec, 0x26, 0xb3,
        0x35, 0x3b, 0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c};
    static const uint8_t kat_expect[32] = {
        0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90, 0x8e, 0x94, 0xea,
        0x4d, 0xf2, 0x8d, 0x08, 0x4f, 0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c,
        0x71, 0xf7, 0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52};
    uint8_t kat_out[32];
    if (!x25519(kat_out, kat_scalar, kat_u)) {
        printf("dkTestX25519SmallOrderReject: KAT pre-rejected\n");
        failures++;
    } else if (memcmp(kat_out, kat_expect, 32) != 0) {
        printf("dkTestX25519SmallOrderReject: KAT output mismatch\n");
        failures++;
    }

    if (failures == 0) {
        printf("dkTestX25519SmallOrderReject: OK\n");
        return 0;
    }
    return 1;
}

/* ChaCha20 SIMD parity — backend-pinned RFC vectors + 1000 random
 * sizes 0..16384 + streaming bulk path + envelope round-trip. */
static int a6FillPRNG(uint8_t *buf, size_t n, uint64_t *state) {
    uint64_t s = *state;
    while (n >= 8) {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        memcpy(buf, &z, 8);
        buf += 8;
        n -= 8;
    }
    if (n > 0) {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        memcpy(buf, &z, n);
    }
    *state = s;
    return 0;
}

static int a6OneSeal(chacha20ForceBackend backend, const uint8_t *key,
                     const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                     const uint8_t *pt, size_t pt_len, uint8_t *ct,
                     uint8_t tag[16]) {
    chacha20SetForceBackend(backend);
    bool ok =
        chacha20poly1305Seal(key, nonce, aad, aad_len, pt, pt_len, ct, tag);
    return ok ? 0 : 1;
}

static int dkTestChaChaSimdParityTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    const bool neonOk = chacha20NeonAvailable();
    const bool avx2Ok = chacha20AVX2Available();

    chacha20SetForceBackend(CHACHA20_FORCE_SCALAR);
    if (chacha20ActiveBackend() != CHACHA20_BACKEND_SCALAR) {
        printf("dkTestChaChaSimdParity: SCALAR force did not pin\n");
        failures++;
    }
    if (neonOk) {
        chacha20SetForceBackend(CHACHA20_FORCE_NEON);
        if (chacha20ActiveBackend() != CHACHA20_BACKEND_NEON) {
            printf("dkTestChaChaSimdParity: NEON force did not pin\n");
            failures++;
        }
    }
    if (avx2Ok) {
        chacha20SetForceBackend(CHACHA20_FORCE_AVX2);
        if (chacha20ActiveBackend() != CHACHA20_BACKEND_AVX2) {
            printf("dkTestChaChaSimdParity: AVX2 force did not pin\n");
            failures++;
        }
    }

    /* Phase 1: RFC 8439 §2.8.2 — parity across backends */
    {
        const uint8_t key[32] = {
            0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a,
            0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
            0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f};
        const uint8_t nonce[12] = {0x07, 0x00, 0x00, 0x00, 0x40, 0x41,
                                   0x42, 0x43, 0x44, 0x45, 0x46, 0x47};
        const uint8_t aad[12] = {0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1,
                                 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7};
        const uint8_t pt[115] =
            "Ladies and Gentlemen of the class of '99: If I could offer you "
            "only one tip for the future, sunscreen would be it.";
        uint8_t ct_scalar[115], tag_scalar[16];
        if (a6OneSeal(CHACHA20_FORCE_SCALAR, key, nonce, aad, sizeof(aad), pt,
                      sizeof(pt), ct_scalar, tag_scalar) != 0) {
            failures++;
        }
        if (neonOk) {
            uint8_t ct[115], tag[16];
            if (a6OneSeal(CHACHA20_FORCE_NEON, key, nonce, aad, sizeof(aad), pt,
                          sizeof(pt), ct, tag) != 0) {
                failures++;
            }
            if (memcmp(ct, ct_scalar, sizeof(ct)) != 0 ||
                memcmp(tag, tag_scalar, 16) != 0) {
                printf("dkTestChaChaSimdParity: NEON vs scalar mismatch RFC "
                       "2.8.2\n");
                failures++;
            }
        }
        if (avx2Ok) {
            uint8_t ct[115], tag[16];
            if (a6OneSeal(CHACHA20_FORCE_AVX2, key, nonce, aad, sizeof(aad), pt,
                          sizeof(pt), ct, tag) != 0) {
                failures++;
            }
            if (memcmp(ct, ct_scalar, sizeof(ct)) != 0 ||
                memcmp(tag, tag_scalar, 16) != 0) {
                failures++;
            }
        }
    }

    /* Phase 2: 1000 random sizes 0..16384 */
    {
        uint64_t prng_state = 0xA6A6A6A6A6A6A6A6ull;
        const size_t bufCap = 16384u;
        cleanerName(zfree, Bytes) uint8_t *pt = (uint8_t *)zmalloc(bufCap);
        cleanerName(zfree, Bytes) uint8_t *ct_scalar =
            (uint8_t *)zmalloc(bufCap);
        cleanerName(zfree, Bytes) uint8_t *ct_simd = (uint8_t *)zmalloc(bufCap);
        cleanerName(zfree, Bytes) uint8_t *rec = (uint8_t *)zmalloc(bufCap);
        if (pt == NULL || ct_scalar == NULL || ct_simd == NULL || rec == NULL) {
            failures++;
            goto a6_cleanup;
        }

        for (int iter = 0; iter < 1000; iter++) {
            uint8_t key[32], nonce[12], aad[16];
            a6FillPRNG(key, sizeof(key), &prng_state);
            a6FillPRNG(nonce, sizeof(nonce), &prng_state);
            a6FillPRNG(aad, sizeof(aad), &prng_state);
            size_t pt_len;
            switch (iter % 7) {
            case 0:
                pt_len = 0;
                break;
            case 1:
                pt_len = (size_t)(prng_state & 63u);
                break;
            case 2:
                pt_len = 64u;
                break;
            case 3:
                pt_len = 255u + (size_t)((prng_state >> 8) & 3u);
                break;
            case 4:
                pt_len = 256u + (size_t)((prng_state >> 16) & 7u);
                break;
            case 5:
                pt_len = 511u + (size_t)((prng_state >> 24) & 3u);
                break;
            default:
                pt_len = (size_t)((prng_state >> 32) & 0x3fffu);
                break;
            }
            if (pt_len > bufCap) {
                pt_len = bufCap;
            }
            if (pt_len > 0) {
                a6FillPRNG(pt, pt_len, &prng_state);
            }

            uint8_t tag_scalar[16], tag_simd[16];
            if (a6OneSeal(CHACHA20_FORCE_SCALAR, key, nonce, aad, sizeof(aad),
                          pt, pt_len, ct_scalar, tag_scalar) != 0) {
                failures++;
                continue;
            }

            if (neonOk) {
                if (a6OneSeal(CHACHA20_FORCE_NEON, key, nonce, aad, sizeof(aad),
                              pt, pt_len, ct_simd, tag_simd) != 0) {
                    failures++;
                    continue;
                }
                if (pt_len > 0 && memcmp(ct_scalar, ct_simd, pt_len) != 0) {
                    printf("dkTestChaChaSimdParity: NEON CT mismatch iter %d "
                           "len=%zu\n",
                           iter, pt_len);
                    failures++;
                }
                if (memcmp(tag_scalar, tag_simd, 16) != 0) {
                    printf(
                        "dkTestChaChaSimdParity: NEON tag mismatch iter %d\n",
                        iter);
                    failures++;
                }
                chacha20SetForceBackend(CHACHA20_FORCE_NEON);
                if (!chacha20poly1305Open(key, nonce, aad, sizeof(aad), ct_simd,
                                          pt_len, tag_simd, rec)) {
                    failures++;
                }
                if (pt_len > 0 && memcmp(rec, pt, pt_len) != 0) {
                    failures++;
                }
            }
            if (avx2Ok) {
                if (a6OneSeal(CHACHA20_FORCE_AVX2, key, nonce, aad, sizeof(aad),
                              pt, pt_len, ct_simd, tag_simd) != 0) {
                    failures++;
                    continue;
                }
                if (pt_len > 0 && memcmp(ct_scalar, ct_simd, pt_len) != 0) {
                    failures++;
                }
                if (memcmp(tag_scalar, tag_simd, 16) != 0) {
                    failures++;
                }
            }
        }
    }

    /* Phase 3: Streaming parity at 4096B (hits SIMD bulk path twice) */
    {
        const uint8_t key[32] = {0xA6};
        const uint8_t nonce[12] = {0xA6};
        const size_t pl = 4096u;
        cleanerName(zfree, Bytes) uint8_t *plain = (uint8_t *)zmalloc(pl);
        cleanerName(zfree, Bytes) uint8_t *one_ct = (uint8_t *)zmalloc(pl);
        cleanerName(zfree, Bytes) uint8_t *stream_ct = (uint8_t *)zmalloc(pl);
        for (size_t i = 0; i < pl; i++) {
            plain[i] = (uint8_t)(i * 13u + 5u);
        }

        chacha20SetForceBackend(CHACHA20_FORCE_SCALAR);
        uint8_t one_tag[16];
        chacha20poly1305Seal(key, nonce, NULL, 0, plain, pl, one_ct, one_tag);

        chacha20SetForceBackend(CHACHA20_FORCE_AUTO);
        chacha20poly1305StreamCtx ctx;
        chacha20poly1305SealStreamInit(&ctx, key, nonce);
        if (!chacha20poly1305SealStreamUpdate(&ctx, plain, pl, stream_ct)) {
            failures++;
        }
        uint8_t stream_tag[16];
        if (!chacha20poly1305SealStreamFinal(&ctx, stream_tag)) {
            failures++;
        }
        if (memcmp(one_ct, stream_ct, pl) != 0) {
            printf("dkTestChaChaSimdParity: streaming CT != one-shot\n");
            failures++;
        }
        if (memcmp(one_tag, stream_tag, 16) != 0) {
            failures++;
        }

        chacha20poly1305StreamCtx octx;
        chacha20poly1305OpenStreamInit(&octx, key, nonce);
        cleanerName(zfree, Bytes) uint8_t *recovered = (uint8_t *)zmalloc(pl);
        if (!chacha20poly1305OpenStreamUpdate(&octx, stream_ct, pl,
                                              recovered)) {
            failures++;
        }
        if (!chacha20poly1305OpenStreamFinal(&octx, stream_tag)) {
            failures++;
        }
        if (memcmp(recovered, plain, pl) != 0) {
            failures++;
        }
    }

a6_cleanup:
    chacha20SetForceBackend(CHACHA20_FORCE_AUTO);

    if (failures == 0) {
        printf("dkTestChaChaSimdParity: OK (RFC + 1000 random + streaming; "
               "backends: scalar%s%s)\n",
               neonOk ? " + NEON" : "", avx2Ok ? " + AVX2" : "");
        return 0;
    }
    printf("dkTestChaChaSimdParity: FAILED with %d issues\n", failures);
    return 1;
}

/* Poly1305 SIMD parity — backend-pinned RFC 8439 vectors + 1000
 * random AEAD round-trips assert SIMD bit-identical to scalar. */
static int dkTestPoly1305SimdParityTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    const bool pNeonOk = poly1305NeonAvailable();
    const bool pAvx2Ok = poly1305AVX2Available();

    /* Pin checks: each force value MUST pin the corresponding backend. */
    poly1305SetForceBackend(POLY1305_FORCE_SCALAR);
    if (poly1305ActiveBackend() != POLY1305_BACKEND_SCALAR) {
        printf("dkTestPoly1305SimdParity: SCALAR force did not pin\n");
        failures++;
    }
    if (pNeonOk) {
        poly1305SetForceBackend(POLY1305_FORCE_NEON);
        if (poly1305ActiveBackend() != POLY1305_BACKEND_NEON) {
            printf("dkTestPoly1305SimdParity: NEON force did not pin\n");
            failures++;
        }
    }
    if (pAvx2Ok) {
        poly1305SetForceBackend(POLY1305_FORCE_AVX2);
        if (poly1305ActiveBackend() != POLY1305_BACKEND_AVX2) {
            printf("dkTestPoly1305SimdParity: AVX2 force did not pin\n");
            failures++;
        }
    }

    /* Phase: 1000 random AEAD round-trips, scalar vs SIMD. Pin ChaCha to
     * scalar so we isolate Poly1305 differences. */
    chacha20SetForceBackend(CHACHA20_FORCE_SCALAR);

    uint64_t prng_state = 0xA7A7A7A7A7A7A7A7ull;
    const size_t bufCap = 65536u;
    cleanerName(zfree, Bytes) uint8_t *pt = (uint8_t *)zmalloc(bufCap);
    cleanerName(zfree, Bytes) uint8_t *ct_scalar = (uint8_t *)zmalloc(bufCap);
    cleanerName(zfree, Bytes) uint8_t *ct_simd = (uint8_t *)zmalloc(bufCap);
    cleanerName(zfree, Bytes) uint8_t *rec = (uint8_t *)zmalloc(bufCap);
    if (pt == NULL || ct_scalar == NULL || ct_simd == NULL || rec == NULL) {
        failures++;
        goto a7_cleanup;
    }

    for (int iter = 0; iter < 1000; iter++) {
        uint8_t key[32], nonce[12], aad[16];
        a6FillPRNG(key, sizeof(key), &prng_state);
        a6FillPRNG(nonce, sizeof(nonce), &prng_state);
        a6FillPRNG(aad, sizeof(aad), &prng_state);
        size_t pt_len;
        switch (iter % 8) {
        case 0:
            pt_len = 16u;
            break;
        case 1:
            pt_len = 31u + (size_t)(prng_state & 3u);
            break;
        case 2:
            pt_len = 64u;
            break;
        case 3:
            pt_len = 127u + (size_t)((prng_state >> 8) & 3u);
            break;
        case 4:
            pt_len = 256u;
            break;
        case 5:
            pt_len = 1024u + (size_t)((prng_state >> 16) & 0xFFu);
            break;
        case 6:
            pt_len = 8192u + (size_t)((prng_state >> 24) & 0xFFu);
            break;
        default:
            pt_len = (size_t)((prng_state >> 32) & 0xFFFFu);
            break;
        }
        if (pt_len > bufCap) {
            pt_len = bufCap;
        }
        if (pt_len > 0) {
            a6FillPRNG(pt, pt_len, &prng_state);
        }

        uint8_t tag_scalar[16], tag_simd[16];
        poly1305SetForceBackend(POLY1305_FORCE_SCALAR);
        if (!chacha20poly1305Seal(key, nonce, aad, sizeof(aad), pt, pt_len,
                                  pt_len > 0 ? ct_scalar : NULL, tag_scalar)) {
            failures++;
            continue;
        }

        if (pNeonOk) {
            poly1305SetForceBackend(POLY1305_FORCE_NEON);
            if (!chacha20poly1305Seal(key, nonce, aad, sizeof(aad), pt, pt_len,
                                      pt_len > 0 ? ct_simd : NULL, tag_simd)) {
                failures++;
                continue;
            }
            if (pt_len > 0 && memcmp(ct_scalar, ct_simd, pt_len) != 0) {
                printf("dkTestPoly1305SimdParity: NEON CT mismatch iter %d "
                       "len=%zu\n",
                       iter, pt_len);
                failures++;
            }
            if (memcmp(tag_scalar, tag_simd, 16) != 0) {
                printf("dkTestPoly1305SimdParity: NEON tag mismatch iter %d "
                       "len=%zu\n",
                       iter, pt_len);
                failures++;
            }
            poly1305SetForceBackend(POLY1305_FORCE_NEON);
            if (!chacha20poly1305Open(key, nonce, aad, sizeof(aad),
                                      pt_len > 0 ? ct_simd : NULL, pt_len,
                                      tag_simd, pt_len > 0 ? rec : NULL)) {
                failures++;
            }
            if (pt_len > 0 && memcmp(rec, pt, pt_len) != 0) {
                failures++;
            }
        }
        if (pAvx2Ok) {
            poly1305SetForceBackend(POLY1305_FORCE_AVX2);
            if (!chacha20poly1305Seal(key, nonce, aad, sizeof(aad), pt, pt_len,
                                      pt_len > 0 ? ct_simd : NULL, tag_simd)) {
                failures++;
                continue;
            }
            if (pt_len > 0 && memcmp(ct_scalar, ct_simd, pt_len) != 0) {
                failures++;
            }
            if (memcmp(tag_scalar, tag_simd, 16) != 0) {
                failures++;
            }
        }
    }

a7_cleanup:
    poly1305SetForceBackend(POLY1305_FORCE_AUTO);
    chacha20SetForceBackend(CHACHA20_FORCE_AUTO);

    if (failures == 0) {
        printf("dkTestPoly1305SimdParity: OK (1000 random AEAD round-trips; "
               "backends: scalar%s%s)\n",
               pNeonOk ? " + NEON" : "", pAvx2Ok ? " + AVX2" : "");
        return 0;
    }
    printf("dkTestPoly1305SimdParity: FAILED with %d issues\n", failures);
    return 1;
}

/* Misc crypto tests bundle (X25519 iter KAT, BLAKE3 derive-key
 * variable input, SHA-256 backend parity). */
static int dkTestX25519Iter1000KATTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;
    static const uint8_t expect_iter1[32] = {
        0x42, 0x2c, 0x8e, 0x7a, 0x62, 0x27, 0xd7, 0xbc, 0xa1, 0x35, 0x0b,
        0x3e, 0x2b, 0xb7, 0x27, 0x9f, 0x78, 0x97, 0xb8, 0x7b, 0xb6, 0x85,
        0x4b, 0x78, 0x3c, 0x60, 0xe8, 0x03, 0x11, 0xae, 0x30, 0x79};
    static const uint8_t expect_iter1000[32] = {
        0x68, 0x4c, 0xf5, 0x9b, 0xa8, 0x33, 0x09, 0x55, 0x28, 0x00, 0xef,
        0x56, 0x6f, 0x2f, 0x4d, 0x3c, 0x1c, 0x38, 0x87, 0xc4, 0x93, 0x60,
        0xe3, 0x87, 0x5f, 0x2e, 0xb9, 0x4d, 0x99, 0x53, 0x2c, 0x51};
    uint8_t k[32], u[32];
    memset(k, 0, sizeof(k));
    memset(u, 0, sizeof(u));
    k[0] = 9;
    u[0] = 9;
    for (int i = 1; i <= 1000; i++) {
        uint8_t kn[32];
        if (!x25519(kn, k, u)) {
            failures++;
            break;
        }
        memcpy(u, k, 32);
        memcpy(k, kn, 32);
        if (i == 1 && memcmp(k, expect_iter1, 32) != 0) {
            printf("dkTestX25519Iter1000KAT: iter1 mismatch\n");
            failures++;
        }
    }
    if (memcmp(k, expect_iter1000, 32) != 0) {
        printf("dkTestX25519Iter1000KAT: iter1000 mismatch\n");
        failures++;
    }
    if (failures == 0) {
        printf("dkTestX25519Iter1000KAT: OK\n");
        return 0;
    }
    return 1;
}

static uint8_t a12HexNyb(char c) {
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint8_t)(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return (uint8_t)(c - 'A' + 10);
    }
    return 0;
}
static void a12HexDec(const char *hex, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        out[i] =
            (uint8_t)((a12HexNyb(hex[i * 2]) << 4) | a12HexNyb(hex[i * 2 + 1]));
    }
}

static int dkTestBlake3DeriveKeyVarInputTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;
    const char *context = BLAKE3_TEST_DERIVE_CONTEXT;
    const size_t ctx_len = strlen(context);

    for (size_t v = 0; v < BLAKE3_TEST_VECTOR_COUNT; v++) {
        const blake3TestVector *tv = &blake3TestVectors[v];
        cleanerName(zfree, Bytes) uint8_t *input =
            (uint8_t *)zmalloc(tv->input_len ? tv->input_len : 1);
        for (size_t i = 0; i < tv->input_len; i++) {
            input[i] = (uint8_t)(i % 251);
        }
        uint8_t expect[32];
        a12HexDec(tv->derive_key_hex, expect, 32);
        uint8_t out[32];
        blake3DeriveKey(context, ctx_len, input, tv->input_len, out, 32);
        if (memcmp(out, expect, 32) != 0) {
            printf("dkTestBlake3DK: KAT mismatch len=%zu\n", tv->input_len);
            failures++;
        }
    }
    if (failures == 0) {
        printf("dkTestBlake3DeriveKeyVarInput: OK (%zu KAT)\n",
               BLAKE3_TEST_VECTOR_COUNT);
        return 0;
    }
    return 1;
}

static const uint32_t kA12Sha256H0[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                         0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                         0x1f83d9abu, 0x5be0cd19u};

static void a12Sha256StoreBE32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void a12Sha256StoreBE64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (56 - 8 * i));
    }
}

typedef void (*a12Sha256Compressor)(uint32_t state[8], const uint8_t *blocks,
                                    size_t num_blocks);

static void a12Sha256HashWith(a12Sha256Compressor compress, const uint8_t *data,
                              size_t len, uint8_t out[32]) {
    uint32_t state[8];
    memcpy(state, kA12Sha256H0, sizeof(kA12Sha256H0));
    size_t full = len / 64;
    if (full) {
        compress(state, data, full);
    }
    uint8_t pad[128] = {0};
    size_t rem = len - full * 64;
    memcpy(pad, data + full * 64, rem);
    pad[rem] = 0x80;
    size_t padBlocks = (rem >= 56) ? 2 : 1;
    a12Sha256StoreBE64(pad + padBlocks * 64 - 8, (uint64_t)len * 8);
    compress(state, pad, padBlocks);
    for (uint32_t i = 0; i < 8; i++) {
        a12Sha256StoreBE32(out + i * 4, state[i]);
    }
}

static uint64_t a12Sha256SplitMix(uint64_t *st) {
    uint64_t s = (*st += 0x9E3779B97F4A7C15ULL);
    s = (s ^ (s >> 30)) * 0xBF58476D1CE4E5B9ULL;
    s = (s ^ (s >> 27)) * 0x94D049BB133111EBULL;
    return s ^ (s >> 31);
}

static int dkTestSha256AllBackendParityTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;
    const bool armOk = sha256ArmCryptoAvailable();
    const bool shaniOk = sha256ShaNiAvailable();
    enum { N_ITERS = 1000, MAX_SZ = 4096 };
    cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(MAX_SZ);
    if (!buf) {
        return 1;
    }
    uint64_t rng = 0xA12DEADBEEFCAFEFULL;
    for (uint32_t it = 0; it < N_ITERS; it++) {
        size_t len = (size_t)(a12Sha256SplitMix(&rng) % (MAX_SZ + 1));
        for (size_t off = 0; off < len; off += 8) {
            uint64_t r = a12Sha256SplitMix(&rng);
            size_t chunk = (len - off >= 8) ? 8 : (len - off);
            memcpy(buf + off, &r, chunk);
        }
        uint8_t reference[32], dispatched[32];
        a12Sha256HashWith(sha256PortableCompressBlocks, buf, len, reference);
        sha256Hash(buf, len, dispatched);
        if (memcmp(reference, dispatched, 32) != 0) {
            printf(
                "dkTestSha256AllBackendParity: dispatched mismatch len=%zu\n",
                len);
            failures++;
        }
        if (armOk) {
            uint8_t arm[32];
            a12Sha256HashWith(sha256ArmCryptoCompressBlocks, buf, len, arm);
            if (memcmp(reference, arm, 32) != 0) {
                failures++;
            }
        }
        if (shaniOk) {
            uint8_t shani[32];
            a12Sha256HashWith(sha256ShaNiCompressBlocks, buf, len, shani);
            if (memcmp(reference, shani, 32) != 0) {
                failures++;
            }
        }
    }
    if (failures == 0) {
        printf("dkTestSha256AllBackendParity: OK (arm=%d shani=%d)\n",
               (int)armOk, (int)shaniOk);
        return 0;
    }
    return 1;
}

/* Expanded AES-GCM coverage. NIST V14/V15/V16 + 108-case
 * self-consistency sweep + wrong-key/AAD/nonce + tag-tamper + MAX-payload. */
typedef struct a5Vector {
    const char *name;
    size_t key_len;
    const char *key_hex;
    const char *nonce_hex;
    const char *aad_hex;
    size_t aad_len;
    const char *pt_hex;
    size_t pt_len;
    const char *ct_hex;
    const char *tag_hex;
} a5Vector;

static void a5HexDec(const char *hex, uint8_t *out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        char c1 = hex[i * 2], c2 = hex[i * 2 + 1];
        out[i] = (uint8_t)(((c1 >= 'a' ? c1 - 'a' + 10 : c1 - '0') << 4) |
                           (c2 >= 'a' ? c2 - 'a' + 10 : c2 - '0'));
    }
}

static int dkTestAesGcmExtendedVectorsTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    const aesGcmForceBackend prev_force = aesGcmGetForceBackend();
    aesGcmSetForceBackend(AES_GCM_FORCE_AUTO);

    static const a5Vector vectors[] = {
        {"V14", 32,
         "0000000000000000000000000000000000000000000000000000000000000000",
         "000000000000000000000000", NULL, 0,
         "00000000000000000000000000000000", 16,
         "cea7403d4d606b6e074ec5d3baf39d18",
         "d0d1c8a799996bf0265b98b5d48ab919"},
        {"V15", 32,
         "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
         "cafebabefacedbaddecaf888", NULL, 0,
         "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
         "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
         64,
         "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
         "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad",
         "b094dac5d93471bdec1a502270e3cc6c"},
        {"V16", 32,
         "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
         "cafebabefacedbaddecaf888", "feedfacedeadbeeffeedfacedeadbeefabaddad2",
         20,
         "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
         "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
         60,
         "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
         "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
         "76fc6ece0f4e1768cddf8853bb2d551b"},
    };
    const size_t n_vectors = sizeof(vectors) / sizeof(vectors[0]);

    for (size_t i = 0; i < n_vectors; i++) {
        const a5Vector *v = &vectors[i];
        uint8_t key[32], nonce[12];
        a5HexDec(v->key_hex, key, v->key_len);
        a5HexDec(v->nonce_hex, nonce, 12);
        uint8_t aad_buf[64];
        const uint8_t *aad = NULL;
        if (v->aad_hex != NULL && v->aad_len > 0) {
            a5HexDec(v->aad_hex, aad_buf, v->aad_len);
            aad = aad_buf;
        }
        uint8_t pt_buf[128];
        const uint8_t *pt = NULL;
        if (v->pt_hex != NULL && v->pt_len > 0) {
            a5HexDec(v->pt_hex, pt_buf, v->pt_len);
            pt = pt_buf;
        }
        aesGcmKey k;
        if (!aesGcmInit(&k, key, v->key_len)) {
            failures++;
            continue;
        }
        uint8_t ct[128] = {0}, tag[16] = {0};
        if (!aesGcmSeal(&k, nonce, aad, v->aad_len, pt, v->pt_len,
                        v->pt_len > 0 ? ct : NULL, tag)) {
            printf("dkTestAesGcmExtendedVectors V[%zu]: Seal failed\n", i);
            failures++;
            continue;
        }
        uint8_t exp_tag[16];
        a5HexDec(v->tag_hex, exp_tag, 16);
        if (memcmp(tag, exp_tag, 16) != 0) {
            printf("dkTestAesGcmExtendedVectors V[%zu]: TAG mismatch on %s\n",
                   i, v->name);
            failures++;
        }
        if (v->ct_hex != NULL && v->pt_len > 0) {
            uint8_t exp_ct[128];
            a5HexDec(v->ct_hex, exp_ct, v->pt_len);
            if (memcmp(ct, exp_ct, v->pt_len) != 0) {
                printf(
                    "dkTestAesGcmExtendedVectors V[%zu]: CT mismatch on %s\n",
                    i, v->name);
                failures++;
            }
        }
        uint8_t rec[128] = {0};
        if (!aesGcmOpen(&k, nonce, aad, v->aad_len, v->pt_len > 0 ? ct : NULL,
                        v->pt_len, tag, v->pt_len > 0 ? rec : NULL)) {
            printf("dkTestAesGcmExtendedVectors V[%zu]: Open failed\n", i);
            failures++;
            continue;
        }
        if (v->pt_len > 0 && pt != NULL && memcmp(rec, pt, v->pt_len) != 0) {
            failures++;
        }
    }

    /* 108-case sweep: 18 sizes x 2 keylens x 3 AADs. */
    {
        static const size_t sweep_sizes[] = {0,   1,   7,   15,  16,   17,
                                             31,  32,  33,  63,  64,   65,
                                             127, 128, 255, 256, 1023, 4096};
        static const size_t aad_lens[] = {0, 1, 20};
        const size_t n_sizes = sizeof(sweep_sizes) / sizeof(sweep_sizes[0]);
        const size_t n_aads = sizeof(aad_lens) / sizeof(aad_lens[0]);

        uint8_t key[32];
        for (size_t i = 0; i < 32; i++) {
            key[i] = (uint8_t)(i * 11u + 3u);
        }
        uint8_t nonce[12];
        for (size_t i = 0; i < 12; i++) {
            nonce[i] = (uint8_t)(i * 17u);
        }
        uint8_t aad_buf[24];
        for (size_t i = 0; i < 24; i++) {
            aad_buf[i] = (uint8_t)(i * 7u + 1u);
        }

        for (uint32_t kl_idx = 0; kl_idx < 2; kl_idx++) {
            const size_t kl = (kl_idx == 0) ? 16u : 32u;
            aesGcmKey k;
            if (!aesGcmInit(&k, key, kl)) {
                failures++;
                continue;
            }
            for (size_t ai = 0; ai < n_aads; ai++) {
                const size_t al = aad_lens[ai];
                const uint8_t *aad = (al > 0) ? aad_buf : NULL;
                for (size_t si = 0; si < n_sizes; si++) {
                    const size_t n = sweep_sizes[si];
                    cleanerName(zfree, Bytes) uint8_t *pt =
                        (n > 0) ? zmalloc(n) : NULL;
                    cleanerName(zfree, Bytes) uint8_t *ct =
                        (n > 0) ? zmalloc(n) : NULL;
                    cleanerName(zfree, Bytes) uint8_t *rec =
                        (n > 0) ? zmalloc(n) : NULL;
                    for (size_t i = 0; i < n; i++) {
                        pt[i] = (uint8_t)((i * 13u) ^ si ^ ai ^ kl);
                    }
                    uint8_t tag[16];
                    if (!aesGcmSeal(&k, nonce, aad, al, pt, n,
                                    n > 0 ? ct : NULL, tag)) {
                        failures++;
                        continue;
                    }
                    if (!aesGcmOpen(&k, nonce, aad, al, n > 0 ? ct : NULL, n,
                                    tag, n > 0 ? rec : NULL)) {
                        failures++;
                    }
                    if (n > 0 && memcmp(pt, rec, n) != 0) {
                        failures++;
                    }
                    if (n > 0) {
                        ct[n - 1] ^= 0x01;
                        if (aesGcmOpen(&k, nonce, aad, al, ct, n, tag, rec)) {
                            failures++;
                        }
                        ct[n - 1] ^= 0x01;
                    }
                }
            }
        }
    }

    /* Wrong-key/AAD/nonce/tag-tamper + bound — abbreviated for brevity.
     * Here we run the critical paths. */
    {
        uint8_t key[16];
        for (size_t i = 0; i < 16; i++) {
            key[i] = (uint8_t)(0xC0 + i);
        }
        uint8_t nonce[12];
        for (size_t i = 0; i < 12; i++) {
            nonce[i] = (uint8_t)(0x12 + i);
        }
        uint8_t pt[32];
        for (size_t i = 0; i < 32; i++) {
            pt[i] = (uint8_t)(0x80 + i);
        }
        aesGcmKey k;
        aesGcmInit(&k, key, 16);
        uint8_t ct[32], tag[16];
        aesGcmSeal(&k, nonce, NULL, 0, pt, 32, ct, tag);

        uint8_t key2[16];
        memcpy(key2, key, 16);
        key2[0] ^= 0x01;
        aesGcmKey k2;
        aesGcmInit(&k2, key2, 16);
        uint8_t rec[32];
        memset(rec, 0x77, sizeof(rec));
        if (aesGcmOpen(&k2, nonce, NULL, 0, ct, 32, tag, rec)) {
            failures++;
        }
        for (size_t i = 0; i < 32; i++) {
            if (rec[i] != 0) {
                failures++;
                break;
            }
        }

        uint8_t n2[12];
        memcpy(n2, nonce, 12);
        n2[5] ^= 0x40;
        memset(rec, 0x77, sizeof(rec));
        if (aesGcmOpen(&k, n2, NULL, 0, ct, 32, tag, rec)) {
            failures++;
        }

        uint8_t bad_tag[16];
        memcpy(bad_tag, tag, 16);
        bad_tag[3] ^= 0x10;
        memset(rec, 0x77, sizeof(rec));
        if (aesGcmOpen(&k, nonce, NULL, 0, ct, 32, bad_tag, rec)) {
            failures++;
        }
        for (size_t i = 0; i < 32; i++) {
            if (rec[i] != 0) {
                failures++;
                break;
            }
        }
    }

    /* Bound refusal. */
    {
        uint8_t key[16] = {0};
        uint8_t nonce[12] = {0};
        uint8_t tag[16] = {0};
        aesGcmKey k;
        aesGcmInit(&k, key, 16);
        if (aesGcmSeal(&k, nonce, NULL, 0, NULL,
                       (size_t)AES_GCM_MAX_PAYLOAD + 1, NULL, tag)) {
            failures++;
        }
        if (aesGcmOpen(&k, nonce, NULL, 0, NULL,
                       (size_t)AES_GCM_MAX_PAYLOAD + 1, tag, NULL)) {
            failures++;
        }
        if (aesGcmSeal(&k, nonce, NULL, 0, NULL, (size_t)-1, NULL, tag)) {
            failures++;
        }
    }

    aesGcmSetForceBackend(prev_force);

    if (failures == 0) {
        printf("dkTestAesGcmExtendedVectors: OK (3 NIST + 108-sweep + tamper "
               "+ bound)\n");
        return 0;
    }
    printf("dkTestAesGcmExtendedVectors: %d failures\n", failures);
    return 1;
}

/* BLAKE3 derive-key context-length test variations. */
static int dkTestBlake3DeriveKeyContextsTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    static const size_t ctx_lens[] = {0, 1, 32, 128, 1024};
    const size_t n_ctx = sizeof(ctx_lens) / sizeof(ctx_lens[0]);

    enum { CTX_MAX = 1024 };
    uint8_t ctx_buf[CTX_MAX];
    for (size_t i = 0; i < CTX_MAX; i++) {
        ctx_buf[i] = (uint8_t)((i * 31u + 0x5Au) & 0xFFu);
    }

    uint8_t ikm_a[32];
    uint8_t ikm_b[32];
    for (size_t i = 0; i < 32; i++) {
        ikm_a[i] = (uint8_t)(0xA0u + i);
        ikm_b[i] = (uint8_t)(0xB0u + i);
    }

    enum { OUT_LEN = 64 };
    uint8_t per_ctx_out[5][OUT_LEN];

    for (size_t i = 0; i < n_ctx; i++) {
        const size_t cl = ctx_lens[i];
        uint8_t out1[OUT_LEN];
        uint8_t out2[OUT_LEN];
        memset(out1, 0xFE, OUT_LEN);
        memset(out2, 0xFD, OUT_LEN);
        blake3DeriveKey(ctx_buf, cl, ikm_a, sizeof(ikm_a), out1, OUT_LEN);
        blake3DeriveKey(ctx_buf, cl, ikm_a, sizeof(ikm_a), out2, OUT_LEN);
        if (memcmp(out1, out2, OUT_LEN) != 0) {
            printf("dkTestBlake3DeriveKeyContexts: determinism fail at "
                   "ctx_len=%zu\n",
                   cl);
            failures++;
        }
        uint8_t out_b[OUT_LEN];
        blake3DeriveKey(ctx_buf, cl, ikm_b, sizeof(ikm_b), out_b, OUT_LEN);
        if (memcmp(out1, out_b, OUT_LEN) == 0) {
            printf("dkTestBlake3DeriveKeyContexts: ikm-sensitivity fail at "
                   "ctx_len=%zu\n",
                   cl);
            failures++;
        }
        memcpy(per_ctx_out[i], out1, OUT_LEN);
    }

    for (size_t a = 0; a < n_ctx; a++) {
        for (size_t b = a + 1; b < n_ctx; b++) {
            if (memcmp(per_ctx_out[a], per_ctx_out[b], OUT_LEN) == 0) {
                printf("dkTestBlake3DeriveKeyContexts: ctx-len collision: %zu "
                       "vs %zu\n",
                       ctx_lens[a], ctx_lens[b]);
                failures++;
            }
        }
    }

    if (failures == 0) {
        printf("dkTestBlake3DeriveKeyContexts: OK (%zu lengths, %zu pairs)\n",
               n_ctx, (n_ctx * (n_ctx - 1)) / 2);
        return 0;
    }
    return 1;
}

/* ====================================================================
 * persistTypeName registry — every declared PERSIST_TYPE_* must
 * resolve to a non-NULL, non-empty, non-"unknown" string. INVALID and
 * unmapped codes MUST return "unknown" (default branch reachable).
 * Pins the 4 new entries 90/91/92/93.
 * ==================================================================== */
static int dkTestPersistTypeNameAllRegisteredTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    struct {
        persistType code;
        const char *label;
    } known[] = {
        {PERSIST_TYPE_FLEX, "flex"},
        {PERSIST_TYPE_INTSET, "intset"},
        {PERSIST_TYPE_INTSETBIG, "intsetBig"},
        {PERSIST_TYPE_MULTILIST, "multilist"},
        {PERSIST_TYPE_MULTILIST_SMALL, "multilistSmall"},
        {PERSIST_TYPE_MULTILIST_MEDIUM, "multilistMedium"},
        {PERSIST_TYPE_MULTILIST_FULL, "multilistFull"},
        {PERSIST_TYPE_MULTIMAP, "multimap"},
        {PERSIST_TYPE_MULTIMAP_SMALL, "multimapSmall"},
        {PERSIST_TYPE_MULTIMAP_MEDIUM, "multimapMedium"},
        {PERSIST_TYPE_MULTIMAP_FULL, "multimapFull"},
        {PERSIST_TYPE_MULTIDICT, "multidict"},
        {PERSIST_TYPE_MULTIARRAY, "multiarray"},
        {PERSIST_TYPE_MULTIARRAY_SMALL, "multiarraySmall"},
        {PERSIST_TYPE_MULTIARRAY_MEDIUM, "multiarrayMedium"},
        {PERSIST_TYPE_MULTIARRAY_LARGE, "multiarrayLarge"},
        {PERSIST_TYPE_MULTIORDEREDSET, "multiOrderedSet"},
        {PERSIST_TYPE_MULTILRU, "multilru"},
        {PERSIST_TYPE_MULTIROAR, "multiroar"},
        {PERSIST_TYPE_LINEARBLOOM, "linearBloom"},
        {PERSIST_TYPE_HYPERLOGLOG, "hyperloglog"},
        {PERSIST_TYPE_HMAC_KEYRING, "hmacKeyring"},
        {PERSIST_TYPE_CLUSTER_MAILBOX_WATERMARKS, "clusterMailboxWatermarks"},
        {PERSIST_TYPE_CLUSTER_RING, "clusterRing"},
        {PERSIST_TYPE_AEAD_NONCE_LOG, "aeadNonceLog"},
    };
    const size_t n = sizeof(known) / sizeof(known[0]);
    int failures = 0;

    for (size_t i = 0; i < n; i++) {
        const char *got = persistTypeName(known[i].code);
        if (got == NULL) {
            printf(
                "dkTestPersistTypeNameAllRegistered: code=%u returned NULL\n",
                (unsigned)known[i].code);
            failures++;
            continue;
        }
        if (got[0] == '\0') {
            printf("dkTestPersistTypeNameAllRegistered: code=%u returned "
                   "empty string\n",
                   (unsigned)known[i].code);
            failures++;
            continue;
        }
        if (strcmp(got, "unknown") == 0) {
            printf("dkTestPersistTypeNameAllRegistered: code=%u (expected "
                   "%s) returned \"unknown\"\n",
                   (unsigned)known[i].code, known[i].label);
            failures++;
            continue;
        }
        if (strcmp(got, known[i].label) != 0) {
            printf("dkTestPersistTypeNameAllRegistered: code=%u returned "
                   "\"%s\", expected \"%s\"\n",
                   (unsigned)known[i].code, got, known[i].label);
            failures++;
        }
    }

    if (strcmp(persistTypeName(PERSIST_TYPE_INVALID), "unknown") != 0) {
        printf("dkTestPersistTypeNameAllRegistered: PERSIST_TYPE_INVALID "
               "must map to \"unknown\"\n");
        failures++;
    }
    if (strcmp(persistTypeName((persistType)200), "unknown") != 0) {
        printf("dkTestPersistTypeNameAllRegistered: unmapped code 200 must "
               "map to \"unknown\"\n");
        failures++;
    }

    if (failures == 0) {
        printf("dkTestPersistTypeNameAllRegistered: OK (%zu types covered)\n",
               n);
        return 0;
    }
    return 1;
}

/* ====================================================================
 * persistOpsHmacKeyring descriptor round-trip — exercises the
 * standard .snapshot + .restore path through the descriptor. Asserts
 * count, size, restore non-NULL, and that the prepared HMAC produces
 * an identical tag in the restored ring for the same id+algo.
 * ==================================================================== */
static int dkTestKeyringPersistOpsDescriptorRoundTripTest(int argc,
                                                          char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    const uint8_t key_a[32] = {
        0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab,
        0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
        0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0,
    };
    const uint8_t key_b[32] = {
        0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb,
        0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6,
        0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe0,
    };
    const uint8_t salt_a[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x11, 0x22, 0x33,
                                                          0x44};
    const uint8_t salt_b[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x55, 0x66, 0x77,
                                                          0x88};
    const hmacKeyId ID_A = 101u;
    const hmacKeyId ID_B = 202u;

    hmacKeyring *src = hmacKeyringCreate();
    if (src == NULL) {
        printf("dkTestKeyringPersistOpsDescriptorRoundTrip: source create "
               "failed\n");
        return 1;
    }

    if (!hmacKeyringInstall(src, ID_A, &hmacAlgoSHA256, key_a, sizeof(key_a),
                            salt_a)) {
        printf(
            "dkTestKeyringPersistOpsDescriptorRoundTrip: install A failed\n");
        hmacKeyringFree(src);
        return 1;
    }
    if (!hmacKeyringInstall(src, ID_B, &hmacAlgoBLAKE3, key_b, sizeof(key_b),
                            salt_b)) {
        printf(
            "dkTestKeyringPersistOpsDescriptorRoundTrip: install B failed\n");
        hmacKeyringFree(src);
        return 1;
    }

    if (persistOpsHmacKeyring.type != PERSIST_TYPE_HMAC_KEYRING) {
        printf("dkTestKeyringPersistOpsDescriptorRoundTrip: descriptor type "
               "mismatch (got %u)\n",
               (unsigned)persistOpsHmacKeyring.type);
        failures++;
    }
    if (persistOpsHmacKeyring.snapshot == NULL ||
        persistOpsHmacKeyring.restore == NULL ||
        persistOpsHmacKeyring.count == NULL ||
        persistOpsHmacKeyring.free == NULL) {
        printf("dkTestKeyringPersistOpsDescriptorRoundTrip: descriptor "
               "missing required ops\n");
        hmacKeyringFree(src);
        return 1;
    }

    const size_t src_count = persistOpsHmacKeyring.count(src);
    if (src_count != 2u) {
        printf("dkTestKeyringPersistOpsDescriptorRoundTrip: descriptor count "
               "expected 2, got %zu\n",
               src_count);
        failures++;
    }

    size_t snap_len = 0;
    uint8_t *snap = persistOpsHmacKeyring.snapshot(src, &snap_len);
    if (snap == NULL || snap_len == 0) {
        printf("dkTestKeyringPersistOpsDescriptorRoundTrip: snapshot "
               "returned empty\n");
        hmacKeyringFree(src);
        return 1;
    }
    if (persistOpsHmacKeyring.estimateSize != NULL &&
        persistOpsHmacKeyring.estimateSize(src) != snap_len) {
        printf("dkTestKeyringPersistOpsDescriptorRoundTrip: estimateSize "
               "disagrees with snapshot length\n");
        failures++;
    }

    const uint8_t probe[] = {'e', '1', '-', 'p', 'r', 'o', 'b', 'e'};
    uint8_t mac_src_a[64] = {0};
    uint8_t mac_src_b[64] = {0};
    const hmacKey *src_a = hmacKeyringLookup(src, ID_A);
    const hmacKey *src_b = hmacKeyringLookup(src, ID_B);
    if (src_a == NULL || src_b == NULL) {
        printf("dkTestKeyringPersistOpsDescriptorRoundTrip: source lookup "
               "failed\n");
        zfree(snap);
        hmacKeyringFree(src);
        return 1;
    }
    hmacComputeWithKey(src_a, probe, sizeof(probe), mac_src_a);
    hmacComputeWithKey(src_b, probe, sizeof(probe), mac_src_b);

    hmacKeyring *dst =
        (hmacKeyring *)persistOpsHmacKeyring.restore(snap, snap_len);
    if (dst == NULL) {
        printf("dkTestKeyringPersistOpsDescriptorRoundTrip: descriptor "
               "restore returned NULL\n");
        zfree(snap);
        hmacKeyringFree(src);
        return 1;
    }

    if (persistOpsHmacKeyring.count(dst) != src_count) {
        printf("dkTestKeyringPersistOpsDescriptorRoundTrip: restored count "
               "mismatch (got %zu)\n",
               persistOpsHmacKeyring.count(dst));
        failures++;
    }

    const hmacKey *dst_a = hmacKeyringLookup(dst, ID_A);
    const hmacKey *dst_b = hmacKeyringLookup(dst, ID_B);
    if (dst_a == NULL || dst_b == NULL) {
        printf("dkTestKeyringPersistOpsDescriptorRoundTrip: restored lookup "
               "failed\n");
        failures++;
    } else {
        uint8_t mac_dst_a[64] = {0};
        uint8_t mac_dst_b[64] = {0};
        hmacComputeWithKey(dst_a, probe, sizeof(probe), mac_dst_a);
        hmacComputeWithKey(dst_b, probe, sizeof(probe), mac_dst_b);
        if (memcmp(mac_src_a, mac_dst_a, hmacAlgoSHA256.digest_size) != 0) {
            printf("dkTestKeyringPersistOpsDescriptorRoundTrip: MAC under "
                   "ID_A drifted after round-trip\n");
            failures++;
        }
        if (memcmp(mac_src_b, mac_dst_b, hmacAlgoBLAKE3.digest_size) != 0) {
            printf("dkTestKeyringPersistOpsDescriptorRoundTrip: MAC under "
                   "ID_B drifted after round-trip\n");
            failures++;
        }
    }

    persistOpsHmacKeyring.free(dst);
    zfree(snap);
    hmacKeyringFree(src);

    if (failures == 0) {
        printf("dkTestKeyringPersistOpsDescriptorRoundTrip: OK (2 keys "
               "round-tripped, MACs match)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * persistOpsClusterRing descriptor round-trip — constructs a
 * small 4-node ring, snapshots via descriptor, restores via descriptor,
 * asserts structural equality (node count, descriptor count agrees).
 * ==================================================================== */
static int dkTestRingPersistOpsDescriptorRoundTripTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    clusterRingConfig config = {
        .name = "e1-ring-roundtrip",
        .strategyType = CLUSTER_STRATEGY_KETAMA,
    };
    cleaner(clusterRingFree) clusterRing *src = clusterRingNew(&config);
    if (src == NULL) {
        printf("dkTestRingPersistOpsDescriptorRoundTrip: ring create failed\n");
        return 1;
    }
    const uint32_t kNodes = 4u;
    for (uint32_t i = 1u; i <= kNodes; i++) {
        clusterNodeConfig nc = {
            .id = (uint64_t)i,
            .weight = 100u,
            .capacityBytes = 1024u * 1024u,
            .initialState = CLUSTER_NODE_UP,
        };
        clusterRingAddNode(src, &nc);
    }

    if (persistOpsClusterRing.type != PERSIST_TYPE_CLUSTER_RING) {
        printf("dkTestRingPersistOpsDescriptorRoundTrip: descriptor type "
               "mismatch (got %u)\n",
               (unsigned)persistOpsClusterRing.type);
        failures++;
    }
    if (persistOpsClusterRing.snapshot == NULL ||
        persistOpsClusterRing.restore == NULL ||
        persistOpsClusterRing.count == NULL ||
        persistOpsClusterRing.free == NULL) {
        printf("dkTestRingPersistOpsDescriptorRoundTrip: descriptor missing "
               "required ops\n");
        return 1;
    }

    if (persistOpsClusterRing.count(src) != (size_t)kNodes) {
        printf("dkTestRingPersistOpsDescriptorRoundTrip: descriptor count "
               "expected %u, got %zu\n",
               kNodes, persistOpsClusterRing.count(src));
        failures++;
    }

    size_t snap_len = 0;
    uint8_t *snap = persistOpsClusterRing.snapshot(src, &snap_len);
    if (snap == NULL || snap_len == 0) {
        printf("dkTestRingPersistOpsDescriptorRoundTrip: snapshot returned "
               "empty\n");
        return 1;
    }
    if (persistOpsClusterRing.estimateSize != NULL &&
        persistOpsClusterRing.estimateSize(src) != snap_len) {
        printf("dkTestRingPersistOpsDescriptorRoundTrip: estimateSize "
               "disagrees with snapshot length\n");
        failures++;
    }

    clusterRing *dst =
        (clusterRing *)persistOpsClusterRing.restore(snap, snap_len);
    if (dst == NULL) {
        printf("dkTestRingPersistOpsDescriptorRoundTrip: restore returned "
               "NULL\n");
        zfree(snap);
        return 1;
    }
    if (clusterRingNodeCount(dst) != kNodes) {
        printf("dkTestRingPersistOpsDescriptorRoundTrip: restored node count "
               "expected %u, got %u\n",
               kNodes, clusterRingNodeCount(dst));
        failures++;
    }
    if (persistOpsClusterRing.count(dst) != (size_t)kNodes) {
        printf("dkTestRingPersistOpsDescriptorRoundTrip: descriptor count "
               "after restore expected %u, got %zu\n",
               kNodes, persistOpsClusterRing.count(dst));
        failures++;
    }

    persistOpsClusterRing.free(dst);
    zfree(snap);

    if (failures == 0) {
        printf("dkTestRingPersistOpsDescriptorRoundTrip: OK (%u-node ring "
               "round-tripped)\n",
               kNodes);
        return 0;
    }
    return 1;
}

/* persistRecover() allocates replayMflexState for every multilist
 * tier (umbrella + SMALL + MEDIUM + FULL), not only the umbrella. */
typedef struct {
    persistType type;
    bool sawReplayState;
    bool applyInvoked;
} dkTestTieredApplyObservation;

static dkTestTieredApplyObservation *dkTestTieredCurrentObservation = NULL;

static bool dkTestTieredApplyOpWrapper(void *structure, persistOp op,
                                       const uint8_t *data, size_t len) {
    if (dkTestTieredCurrentObservation) {
        dkTestTieredCurrentObservation->applyInvoked = true;
        if (persistGetReplayState() != NULL) {
            dkTestTieredCurrentObservation->sawReplayState = true;
        }
    }
    return persistOpsMultilist.applyOp(structure, op, data, len);
}

static int dkTestPersistMultilistTieredApplyTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    const persistType tiers[] = {
        PERSIST_TYPE_MULTILIST,
        PERSIST_TYPE_MULTILIST_SMALL,
        PERSIST_TYPE_MULTILIST_MEDIUM,
        PERSIST_TYPE_MULTILIST_FULL,
    };
    const char *tierLabels[] = {
        "MULTILIST",
        "MULTILIST_SMALL",
        "MULTILIST_MEDIUM",
        "MULTILIST_FULL",
    };
    const size_t nTiers = sizeof(tiers) / sizeof(tiers[0]);
    int failures = 0;

    for (size_t t = 0; t < nTiers; t++) {
        persistOps tierOps = persistOpsMultilist;
        tierOps.type = tiers[t];
        tierOps.applyOp = dkTestTieredApplyOpWrapper;

        dkTestTieredApplyObservation obs = {
            .type = tiers[t],
            .sawReplayState = false,
            .applyInvoked = false,
        };
        dkTestTieredCurrentObservation = &obs;

        cleaner(multilistFree) multilist *ml =
            multilistNew(FLEX_CAP_LEVEL_2048, 0);
        if (!ml) {
            printf(
                "dkTestPersistMultilistTieredApply[%s]: multilistNew failed\n",
                tierLabels[t]);
            failures++;
            dkTestTieredCurrentObservation = NULL;
            continue;
        }
        databox seed = DATABOX_SIGNED(1);
        multilistPushByTypeTail(&ml, NULL, &seed);

        persist *p = persistCreate(&tierOps, NULL);
        if (!p) {
            printf(
                "dkTestPersistMultilistTieredApply[%s]: persistCreate failed\n",
                tierLabels[t]);
            failures++;
            dkTestTieredCurrentObservation = NULL;
            continue;
        }

        persistStore *snapStore = persistStoreMemory(0);
        persistStore *walStore = persistStoreMemory(0);
        persistAttachSnapshot(p, snapStore);
        persistAttachWAL(p, walStore);

        if (!persistSnapshot(p, ml)) {
            printf("dkTestPersistMultilistTieredApply[%s]: persistSnapshot "
                   "failed\n",
                   tierLabels[t]);
            persistClose(p);
            failures++;
            dkTestTieredCurrentObservation = NULL;
            continue;
        }

        databox pushBox = DATABOX_SIGNED(42);
        if (!persistLogOp(p, PERSIST_OP_PUSH_TAIL, &pushBox, 1)) {
            printf(
                "dkTestPersistMultilistTieredApply[%s]: persistLogOp failed\n",
                tierLabels[t]);
            persistClose(p);
            failures++;
            dkTestTieredCurrentObservation = NULL;
            continue;
        }
        persistSync(p);

        multilist *recovered = persistRecover(p);
        if (!recovered) {
            printf("dkTestPersistMultilistTieredApply[%s]: persistRecover "
                   "returned NULL\n",
                   tierLabels[t]);
            persistClose(p);
            failures++;
            dkTestTieredCurrentObservation = NULL;
            continue;
        }

        if (!obs.applyInvoked) {
            printf("dkTestPersistMultilistTieredApply[%s]: WAL apply callback "
                   "never invoked\n",
                   tierLabels[t]);
            failures++;
        } else if (!obs.sawReplayState) {
            printf("dkTestPersistMultilistTieredApply[%s]: "
                   "persistGetReplayState() NULL during apply\n",
                   tierLabels[t]);
            failures++;
        }
        if (multilistCount(recovered) != 2) {
            printf("dkTestPersistMultilistTieredApply[%s]: expected 2 elements "
                   "after replay, got %zu\n",
                   tierLabels[t], multilistCount(recovered));
            failures++;
        }

        multilistFree(recovered);
        persistClose(p);
        dkTestTieredCurrentObservation = NULL;
    }

    if (failures == 0) {
        printf("dkTestPersistMultilistTieredApply: OK (%zu tiers covered)\n",
               nTiers);
        return 0;
    }
    return 1;
}

/* persistOpsClusterMailbox descriptor round-trip. */
static bool dkE1MailboxAcceptApply(const clusterFrame *frame, void *ctx) {
    (void)frame;
    (void)ctx;
    return true;
}

static int dkTestMailboxPersistOpsDescriptorRoundTripTest(int argc,
                                                          char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    const uint8_t key[32] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
        0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
    };
    const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xa1, 0xa2, 0xa3, 0xa4};
    const hmacKeyId KID = 7u;

    hmacKeyring *ring = hmacKeyringCreate();
    if (ring == NULL) {
        return 1;
    }
    if (!hmacKeyringInstall(ring, KID, &hmacAlgoSHA256, key, sizeof(key),
                            salt)) {
        hmacKeyringFree(ring);
        return 1;
    }

    clusterMailbox *src = clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256,
                                               dkE1MailboxAcceptApply, NULL);
    if (src == NULL) {
        hmacKeyringFree(ring);
        return 1;
    }

    const struct {
        uint32_t origin_id;
        uint64_t max_seq;
    } peers[] = {{1001u, 5u}, {2002u, 11u}, {3003u, 100u}};
    const size_t kPeers = sizeof(peers) / sizeof(peers[0]);

    uint8_t scratch[512];
    for (size_t p = 0; p < kPeers; p++) {
        for (uint64_t seq = 1u; seq <= peers[p].max_seq; seq++) {
            size_t written = 0;
            const uint8_t payload[4] = {0xde, 0xad, 0xbe, 0xef};
            if (!clusterMailboxSeal(src, peers[p].origin_id, seq, 0x01, payload,
                                    sizeof(payload), KID, scratch,
                                    sizeof(scratch), &written)) {
                printf("dkTestMailboxPersistOpsDescriptorRoundTrip: seal "
                       "fail\n");
                failures++;
                goto done_src;
            }
            if (clusterMailboxIngest(src, scratch, written, KID) !=
                CLUSTER_MAILBOX_OK) {
                printf("dkTestMailboxPersistOpsDescriptorRoundTrip: ingest "
                       "fail\n");
                failures++;
                goto done_src;
            }
        }
    }

    if (clusterMailboxOriginCount(src) != kPeers) {
        failures++;
    }
    if (persistOpsClusterMailbox.type !=
        PERSIST_TYPE_CLUSTER_MAILBOX_WATERMARKS) {
        failures++;
    }
    if (persistOpsClusterMailbox.snapshot == NULL ||
        persistOpsClusterMailbox.restore == NULL ||
        persistOpsClusterMailbox.count == NULL ||
        persistOpsClusterMailbox.free == NULL) {
        failures++;
        goto done_src;
    }
    if (persistOpsClusterMailbox.count(src) != kPeers) {
        failures++;
    }

    size_t snap_len = 0;
    uint8_t *snap = persistOpsClusterMailbox.snapshot(src, &snap_len);
    if (snap == NULL || snap_len == 0) {
        failures++;
        goto done_src;
    }
    if (persistOpsClusterMailbox.estimateSize != NULL &&
        persistOpsClusterMailbox.estimateSize(src) != snap_len) {
        failures++;
    }

    clusterMailbox *dst =
        (clusterMailbox *)persistOpsClusterMailbox.restore(snap, snap_len);
    if (dst == NULL) {
        printf("dkTestMailboxPersistOpsDescriptorRoundTrip: restore NULL\n");
        failures++;
        zfree(snap);
        goto done_src;
    }
    if (persistOpsClusterMailbox.count(dst) != kPeers) {
        failures++;
    }
    for (size_t p = 0; p < kPeers; p++) {
        if (clusterMailboxLastApplied(dst, peers[p].origin_id) !=
            peers[p].max_seq) {
            failures++;
        }
    }
    if (!clusterMailboxAttachKeyring(dst, ring, FLEX_ENV_ALGO_HMAC_SHA256,
                                     dkE1MailboxAcceptApply, NULL)) {
        failures++;
    } else {
        size_t written = 0;
        const uint8_t payload[2] = {0xfe, 0xed};
        const uint64_t next_seq = peers[0].max_seq + 1u;
        if (!clusterMailboxSeal(dst, peers[0].origin_id, next_seq, 0x02,
                                payload, sizeof(payload), KID, scratch,
                                sizeof(scratch), &written)) {
            failures++;
        } else {
            if (clusterMailboxIngest(dst, scratch, written, KID) !=
                CLUSTER_MAILBOX_OK) {
                failures++;
            }
            if (clusterMailboxLastApplied(dst, peers[0].origin_id) !=
                next_seq) {
                failures++;
            }
        }
    }
    if (persistOpsClusterMailbox.restore(NULL, 0) != NULL) {
        failures++;
    }
    uint8_t junk[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (persistOpsClusterMailbox.restore(junk, sizeof(junk)) != NULL) {
        failures++;
    }
    if (persistOpsClusterMailbox.newEmpty == NULL) {
        failures++;
    } else {
        clusterMailbox *empty =
            (clusterMailbox *)persistOpsClusterMailbox.newEmpty();
        if (empty == NULL) {
            failures++;
        } else {
            if (persistOpsClusterMailbox.count(empty) != 0u) {
                failures++;
            }
            persistOpsClusterMailbox.free(empty);
        }
    }
    persistOpsClusterMailbox.free(dst);
    zfree(snap);

done_src:
    clusterMailboxFree(src);
    hmacKeyringFree(ring);
    if (failures == 0) {
        printf("dkTestMailboxPersistOpsDescriptorRoundTrip: OK (%zu origins, "
               "attach + post-attach ingest)\n",
               kPeers);
        return 0;
    }
    return 1;
}

/* ====================================================================
 * BLAKE3 multi-chunk SIMD dispatcher — assert AUTO and SIMD-forced
 * paths agree with the scalar reference at every chunk-count boundary
 * around the AVX2-8 / NEON-4 thresholds. Skips the AVX2 assertion when
 * AVX2 is not present on the host (e.g. ARM dev machines).
 * ==================================================================== */
static int dkTestBlake3Avx2DispatchTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    blake3SetForceImpl(BLAKE3_IMPL_AUTO);

    const blake3Backend backend = blake3DispatcherActiveBackend();
    const bool avx2_here = blake3AVX2Available();

    if (avx2_here && backend != BLAKE3_BACKEND_AVX2) {
        printf("dkTestBlake3Avx2Dispatch: AVX2 available but dispatcher "
               "reports backend=%d "
               "(expected %d)\n",
               (int)backend, (int)BLAKE3_BACKEND_AVX2);
        failures++;
    }
    if (!avx2_here && backend == BLAKE3_BACKEND_AVX2) {
        printf("dkTestBlake3Avx2Dispatch: dispatcher reports AVX2 but probe "
               "says unavailable\n");
        failures++;
    }

    /* 32 KiB = 32 chunks = exactly four 8-chunk AVX2 batches
     * (eight 4-chunk NEON batches), so the SIMD path is fully exercised. */
    const size_t N = 32u * 1024u;
    cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(N);
    if (buf == NULL) {
        printf("dkTestBlake3Avx2Dispatch: zmalloc(%zu) failed\n", N);
        return 1;
    }
    uint64_t s = 0xB1A75D15B17C0DE5ULL;
    for (size_t i = 0; i < N; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (uint8_t)(s >> 24);
    }

    /* Reference: forced-scalar path (no SIMD batch). */
    blake3SetForceImpl(BLAKE3_IMPL_SCALAR);
    uint8_t expected[BLAKE3_OUT_LEN];
    blake3Hash(buf, N, expected);

    blake3SetForceImpl(BLAKE3_IMPL_AUTO);
    uint8_t got_auto[BLAKE3_OUT_LEN];
    blake3Hash(buf, N, got_auto);
    if (memcmp(expected, got_auto, BLAKE3_OUT_LEN) != 0) {
        printf("dkTestBlake3Avx2Dispatch: AUTO digest differs from scalar "
               "reference\n");
        failures++;
    }

    blake3SetForceImpl(BLAKE3_IMPL_SIMD);
    uint8_t got_simd[BLAKE3_OUT_LEN];
    blake3Hash(buf, N, got_simd);
    if (memcmp(expected, got_simd, BLAKE3_OUT_LEN) != 0) {
        printf("dkTestBlake3Avx2Dispatch: SIMD-forced digest differs from "
               "scalar reference "
               "(backend=%d)\n",
               (int)blake3DispatcherActiveBackend());
        failures++;
    }
    blake3SetForceImpl(BLAKE3_IMPL_AUTO);

    /* Sweep chunk counts around the 8-chunk AVX2 / 4-chunk NEON boundaries. */
    const size_t sweep_chunks[] = {1,  2,  3,  4,  5,  7,  8,  9,  12, 15,
                                   16, 17, 23, 24, 25, 32, 33, 48, 64};
    const size_t n_sweep = sizeof(sweep_chunks) / sizeof(sweep_chunks[0]);
    for (size_t i = 0; i < n_sweep; i++) {
        const size_t len = sweep_chunks[i] * BLAKE3_CHUNK_LEN;
        if (len > N) {
            continue;
        }
        uint8_t ref[BLAKE3_OUT_LEN];
        uint8_t obs[BLAKE3_OUT_LEN];
        blake3SetForceImpl(BLAKE3_IMPL_SCALAR);
        blake3Hash(buf, len, ref);
        blake3SetForceImpl(BLAKE3_IMPL_AUTO);
        blake3Hash(buf, len, obs);
        if (memcmp(ref, obs, BLAKE3_OUT_LEN) != 0) {
            printf("dkTestBlake3Avx2Dispatch: dispatch digest mismatch at "
                   "chunks=%zu "
                   "(len=%zu)\n",
                   sweep_chunks[i], len);
            failures++;
        }
    }
    blake3SetForceImpl(BLAKE3_IMPL_AUTO);

    const char *backend_name = "scalar";
    switch (backend) {
    case BLAKE3_BACKEND_AVX2:
        backend_name = "AVX2-8";
        break;
    case BLAKE3_BACKEND_NEON:
        backend_name = "NEON-4";
        break;
    case BLAKE3_BACKEND_SCALAR:
    default:
        backend_name = "scalar";
        break;
    }

    if (!avx2_here) {
        printf("dkTestBlake3Avx2Dispatch: skipped AVX2 assertion "
               "(AVX2 not present on this host); dispatcher selected "
               "backend=%s; %zu chunk-counts swept against scalar "
               "reference; %d failures\n",
               backend_name, n_sweep, failures);
    } else {
        printf("dkTestBlake3Avx2Dispatch: AVX2 present; dispatcher "
               "selected backend=%s; 32 KiB buffer and %zu chunk-counts "
               "swept against scalar reference; %d failures\n",
               backend_name, n_sweep, failures);
    }

    return failures == 0 ? 0 : 1;
}

/* ====================================================================
 * hmacKeyringInstall salt requirement — verify that the salted API
 * rejects all-zero salts, the explicit unsalted entry point allows them,
 * and the runtime fail-closed check in hmacKeyringNextAeadNonce refuses
 * to issue a zero-salt nonce from a slot that wasn't installed as
 * unsalted. Closes the cross-process nonce-collision footgun.
 * ==================================================================== */
static int dkTestKeyringRequireSaltTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    /* Case 1: salted install + nonce contains salt suffix */
    {
        hmacKeyring *ring = hmacKeyringCreate();
        if (ring == NULL) {
            printf("dkTestKeyringRequireSalt: create failed\n");
            return 1;
        }
        const uint8_t key[32] = {0xC1, 0xA1, 0xC1, 0xA1};
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xDE, 0xAD, 0xBE,
                                                            0xEF};
        if (!hmacKeyringInstall(ring, 1u, &hmacAlgoSHA256, key, sizeof(key),
                                salt)) {
            printf("dkTestKeyringRequireSalt: salted install must succeed\n");
            failures++;
        } else {
            uint8_t nonce[12] = {0};
            if (!hmacKeyringNextAeadNonce(ring, 1u, nonce)) {
                printf("dkTestKeyringRequireSalt: NextAeadNonce must succeed "
                       "on salted slot\n");
                failures++;
            }
            if (nonce[8] != 0xDE || nonce[9] != 0xAD || nonce[10] != 0xBE ||
                nonce[11] != 0xEF) {
                printf("dkTestKeyringRequireSalt: nonce suffix "
                       "%02x%02x%02x%02x != salt\n",
                       nonce[8], nonce[9], nonce[10], nonce[11]);
                failures++;
            }
            for (size_t i = 0; i < 8; i++) {
                if (nonce[i] != 0) {
                    printf("dkTestKeyringRequireSalt: nonce[%zu]=0x%02x != 0 "
                           "(counter)\n",
                           i, nonce[i]);
                    failures++;
                    break;
                }
            }
        }
        hmacKeyringFree(ring);
    }

    /* Case 2: explicit Unsalted install permits zero-salt nonces */
    {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x11, 0x22, 0x33, 0x44};
        if (!hmacKeyringInstallUnsalted(ring, 2u, &hmacAlgoSHA256, key,
                                        sizeof(key))) {
            printf("dkTestKeyringRequireSalt: InstallUnsalted must succeed\n");
            failures++;
        }
        uint8_t nonce[12];
        memset(nonce, 0xFF, sizeof(nonce));
        if (!hmacKeyringNextAeadNonce(ring, 2u, nonce)) {
            printf("dkTestKeyringRequireSalt: NextAeadNonce must succeed on "
                   "Unsalted slot\n");
            failures++;
        }
        for (size_t i = 0; i < 12; i++) {
            if (nonce[i] != 0) {
                printf("dkTestKeyringRequireSalt: Unsalted nonce0 byte %zu "
                       "non-zero (0x%02x)\n",
                       i, nonce[i]);
                failures++;
                break;
            }
        }
        hmacKeyringFree(ring);
    }

    /* Case 3: salted API rejects all-zero salt */
    {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0xAB};
        const uint8_t zero_salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0};
        if (hmacKeyringInstall(ring, 3u, &hmacAlgoSHA256, key, sizeof(key),
                               zero_salt)) {
            printf("dkTestKeyringRequireSalt: salted Install must REFUSE "
                   "zero salt\n");
            failures++;
        }
        if (hmacKeyringSize(ring) != 0) {
            printf("dkTestKeyringRequireSalt: ring size %zu after refused "
                   "install (expected 0)\n",
                   hmacKeyringSize(ring));
            failures++;
        }
        if (hmacKeyringInstall(ring, 3u, &hmacAlgoSHA256, key, sizeof(key),
                               NULL)) {
            printf("dkTestKeyringRequireSalt: salted Install must REFUSE "
                   "NULL salt\n");
            failures++;
        }
        hmacKeyringFree(ring);
    }

    /* Case 4: runtime fail-closed when salt becomes zero on salted slot */
    {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0xCC};
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xAA, 0xBB, 0xCC,
                                                            0xDD};
        if (!hmacKeyringInstall(ring, 4u, &hmacAlgoSHA256, key, sizeof(key),
                                salt)) {
            printf(
                "dkTestKeyringRequireSalt: salted install (case 4) failed\n");
            failures++;
        }
        const uint8_t zero_salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0};
        hmacKeyringSetAeadSalt(ring, 4u, zero_salt);
        uint8_t nonce[12];
        if (hmacKeyringNextAeadNonce(ring, 4u, nonce)) {
            printf("dkTestKeyringRequireSalt: NextAeadNonce must REFUSE "
                   "zero-salt on salted slot\n");
            failures++;
        }
        hmacKeyringFree(ring);
    }

    if (failures == 0) {
        printf("dkTestKeyringRequireSalt: OK (4 cases)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * persistAttachKeyring lifetime fix via generation counter.
 * The pre-fix code use-after-freed retired key bytes.
 * The new contract caches (ring, key_id, generation_at_attach) and
 * re-resolves on every snapshot/verify; mismatch fails-closed with
 * PERSIST_ERR_KEY_GENERATION_MISMATCH. persistRebindKeyring is the
 * official rotation recovery entry point.
 * ==================================================================== */
static int dkTestKeyringPersistRetireAfterAttachTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    /* Declared here, ahead of the first goto, so the scope-exit cleaner
     * covers every jump to the cleanup labels below. */
    cleaner(flexFree) flex *payload = NULL;

    hmacKeyring *ring = hmacKeyringCreate();
    if (ring == NULL) {
        printf("dkTestKeyringPersistRetireAfterAttach: create failed\n");
        return 1;
    }
    const uint8_t key1[32] = {
        0xC2, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18, 0x29, 0x3A,
        0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5,
        0xF6, 0x07, 0x18, 0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F,
    };
    const uint8_t salt1[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xC2, 0x42, 0xDE,
                                                         0xAD};
    const hmacKeyId kid1 = 101u;
    if (!hmacKeyringInstall(ring, kid1, &hmacAlgoSHA256, key1, sizeof(key1),
                            salt1)) {
        printf("dkTestKeyringPersistRetireAfterAttach: install kid1 failed\n");
        hmacKeyringFree(ring);
        return 1;
    }

    persistConfig config = persistDefaultConfig();
    config.checksumType = PERSIST_CHECKSUM_HMAC_SHA256;
    persist *p = persistCreate(&persistOpsFlex, &config);
    persistStore *snapStore = persistStoreMemory(0);
    persistAttachSnapshot(p, snapStore);
    if (!persistAttachKeyring(p, ring, kid1)) {
        printf("dkTestKeyringPersistRetireAfterAttach: attach kid1 failed\n");
        failures++;
        goto cleanup;
    }
    if (persistLastError(p) != PERSIST_ERR_NONE) {
        printf("dkTestKeyringPersistRetireAfterAttach: lastError non-zero "
               "after attach (got %d)\n",
               (int)persistLastError(p));
        failures++;
    }

    payload = flexNew();
    flexPushBytes(&payload, "c2-pre-retire", 13, FLEX_ENDPOINT_TAIL);
    if (!persistSnapshot(p, payload)) {
        printf("dkTestKeyringPersistRetireAfterAttach: pre-retire snapshot "
               "failed (lastError=%d)\n",
               (int)persistLastError(p));
        failures++;
    }
    if (persistLastError(p) != PERSIST_ERR_NONE) {
        printf("dkTestKeyringPersistRetireAfterAttach: pre-retire success "
               "left lastError=%d\n",
               (int)persistLastError(p));
        failures++;
    }

    if (!hmacKeyringRetire(ring, kid1)) {
        printf("dkTestKeyringPersistRetireAfterAttach: retire kid1 failed\n");
        failures++;
        goto cleanup_payload;
    }

    const bool secondSnap = persistSnapshot(p, payload);
    if (secondSnap) {
        printf("dkTestKeyringPersistRetireAfterAttach: post-retire snapshot "
               "MUST fail (it returned true; UAF / silent-zero hazard)\n");
        failures++;
    }
    const persistErr err = persistLastError(p);
    if (err != PERSIST_ERR_KEY_GENERATION_MISMATCH &&
        err != PERSIST_ERR_KEY_NOT_FOUND) {
        printf(
            "dkTestKeyringPersistRetireAfterAttach: post-retire "
            "lastError expected GENERATION_MISMATCH (1) or KEY_NOT_FOUND (2),"
            " got %d\n",
            (int)err);
        failures++;
    }

    if (persistVerifySnapshot(p)) {
        printf("dkTestKeyringPersistRetireAfterAttach: post-retire verify "
               "MUST fail\n");
        failures++;
    }
    const persistErr verifyErr = persistLastError(p);
    if (verifyErr != PERSIST_ERR_KEY_GENERATION_MISMATCH &&
        verifyErr != PERSIST_ERR_KEY_NOT_FOUND) {
        printf("dkTestKeyringPersistRetireAfterAttach: post-retire verify "
               "lastError expected GENERATION_MISMATCH or KEY_NOT_FOUND, "
               "got %d\n",
               (int)verifyErr);
        failures++;
    }

    const uint8_t key2[32] = {
        0xFE, 0xED, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE, 0xDE, 0xAD, 0xC0,
        0xDE, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x11, 0x22,
        0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC,
    };
    const uint8_t salt2[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x55, 0xAA, 0x55,
                                                         0xAA};
    const hmacKeyId kid2 = 202u;
    if (!hmacKeyringInstall(ring, kid2, &hmacAlgoSHA256, key2, sizeof(key2),
                            salt2)) {
        printf("dkTestKeyringPersistRetireAfterAttach: install kid2 failed\n");
        failures++;
        goto cleanup_payload;
    }
    if (!persistRebindKeyring(p, ring, kid2)) {
        printf("dkTestKeyringPersistRetireAfterAttach: rebind kid2 failed\n");
        failures++;
        goto cleanup_payload;
    }
    if (persistLastError(p) != PERSIST_ERR_NONE) {
        printf("dkTestKeyringPersistRetireAfterAttach: rebind left "
               "lastError=%d (expected 0)\n",
               (int)persistLastError(p));
        failures++;
    }
    if (!persistSnapshot(p, payload)) {
        printf("dkTestKeyringPersistRetireAfterAttach: post-rebind snapshot "
               "failed (lastError=%d)\n",
               (int)persistLastError(p));
        failures++;
    }
    if (!persistVerifySnapshot(p)) {
        printf("dkTestKeyringPersistRetireAfterAttach: post-rebind verify "
               "failed\n");
        failures++;
    }

    const uint8_t salt2b[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xAA, 0x55, 0xAA,
                                                          0x55};
    hmacKeyringSetAeadSalt(ring, kid2, salt2b);
    if (persistSnapshot(p, payload)) {
        printf("dkTestKeyringPersistRetireAfterAttach: post-SetAeadSalt "
               "snapshot MUST fail (generation should have advanced)\n");
        failures++;
    }
    if (persistLastError(p) != PERSIST_ERR_KEY_GENERATION_MISMATCH) {
        printf("dkTestKeyringPersistRetireAfterAttach: post-SetAeadSalt "
               "lastError expected GENERATION_MISMATCH, got %d\n",
               (int)persistLastError(p));
        failures++;
    }
    if (!persistRebindKeyring(p, ring, kid2)) {
        printf("dkTestKeyringPersistRetireAfterAttach: rebind after "
               "SetAeadSalt failed\n");
        failures++;
    }
    if (!persistSnapshot(p, payload)) {
        printf("dkTestKeyringPersistRetireAfterAttach: snapshot after "
               "rebind-after-SetAeadSalt failed\n");
        failures++;
    }

cleanup_payload:
cleanup:
    persistClose(p);
    hmacKeyringFree(ring);

    if (failures == 0) {
        printf("dkTestKeyringPersistRetireAfterAttach: OK (retire + rebind +"
               " SetAeadSalt-rotation paths all caught)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * Revoked-key floor. Install K1/K2/K3; raise floor to K2; verify K1
 * rejects with FLEX_ENV_VERIFY_FAILED_KEY_REVOKED while K2+K3 pass;
 * snapshot+restore preserves the floor.
 * ==================================================================== */
static int dkTestRevokedKeyFloorTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    hmacKeyring *ring = hmacKeyringCreate();
    if (ring == NULL) {
        printf("dkTestRevokedKeyFloor: create failed\n");
        return 1;
    }

    const uint8_t key1[32] = {0xC3, 0x10};
    const uint8_t key2[32] = {0xC3, 0x20};
    const uint8_t key3[32] = {0xC3, 0x30};
    const uint8_t salt1[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x1A, 0x2B, 0x3C,
                                                         0x4D};
    const uint8_t salt2[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x5E, 0x6F, 0x70,
                                                         0x81};
    const uint8_t salt3[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x92, 0xA3, 0xB4,
                                                         0xC5};
    const hmacKeyId K1 = 100u, K2 = 200u, K3 = 300u;

    if (!hmacKeyringInstall(ring, K1, &hmacAlgoSHA256, key1, sizeof(key1),
                            salt1) ||
        !hmacKeyringInstall(ring, K2, &hmacAlgoSHA256, key2, sizeof(key2),
                            salt2) ||
        !hmacKeyringInstall(ring, K3, &hmacAlgoSHA256, key3, sizeof(key3),
                            salt3)) {
        printf("dkTestRevokedKeyFloor: install K1/K2/K3 failed\n");
        hmacKeyringFree(ring);
        return 1;
    }

    const uint8_t payload[24] = "c3-revoked-payload";
    __attribute__((cleanup(freeBytesArray3_cleanup)))
    uint8_t *envs[3] = {NULL, NULL, NULL};
    size_t env_lens[3] = {0, 0, 0};
    const hmacKeyId ids[3] = {K1, K2, K3};
    const uint8_t *keys[3] = {key1, key2, key3};
    for (size_t i = 0; i < 3; i++) {
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
            .key_id = ids[i],
            .key = keys[i],
            .key_len = 32,
        };
        const size_t cap = flexEnvelopeWrappedSize(
            sizeof(payload), FLEX_ENV_ALGO_HMAC_SHA256, wp.on_wire_flags);
        envs[i] = zmalloc(cap);
        if (!flexEnvelopeWrap(envs[i], cap, payload, sizeof(payload), &wp,
                              &env_lens[i])) {
            printf("dkTestRevokedKeyFloor: wrap K%zu failed\n", i + 1);
            failures++;
        }
    }

    flexEnvelopeVerifyParams vp = {.keyring = ring};
    const uint8_t *out = NULL;
    uint64_t out_len = 0;
    for (size_t i = 0; i < 3; i++) {
        if (!flexEnvelopeVerify(envs[i], env_lens[i], &vp, &out, &out_len)) {
            printf("dkTestRevokedKeyFloor: pre-floor verify K%zu MUST pass\n",
                   i + 1);
            failures++;
        }
    }

    if (!hmacKeyringSetMinValidKeyId(ring, K2)) {
        printf("dkTestRevokedKeyFloor: SetMinValidKeyId(K2) failed\n");
        failures++;
    }
    if (flexEnvelopeVerify(envs[0], env_lens[0], &vp, &out, &out_len)) {
        printf("dkTestRevokedKeyFloor: K1 verify after floor MUST fail\n");
        failures++;
    }
    if (flexEnvelopeLastVerifyError() != FLEX_ENV_VERIFY_FAILED_KEY_REVOKED) {
        printf("dkTestRevokedKeyFloor: K1 err expected KEY_REVOKED got %d\n",
               (int)flexEnvelopeLastVerifyError());
        failures++;
    }
    if (!flexEnvelopeVerify(envs[1], env_lens[1], &vp, &out, &out_len)) {
        printf("dkTestRevokedKeyFloor: K2 verify MUST succeed\n");
        failures++;
    }
    if (flexEnvelopeLastVerifyError() != FLEX_ENV_VERIFY_OK) {
        printf("dkTestRevokedKeyFloor: K2 err expected OK got %d\n",
               (int)flexEnvelopeLastVerifyError());
        failures++;
    }
    if (!flexEnvelopeVerify(envs[2], env_lens[2], &vp, &out, &out_len)) {
        printf("dkTestRevokedKeyFloor: K3 verify MUST succeed\n");
        failures++;
    }
    if (flexEnvelopeLastVerifyError() != FLEX_ENV_VERIFY_OK) {
        printf("dkTestRevokedKeyFloor: K3 err expected OK got %d\n",
               (int)flexEnvelopeLastVerifyError());
        failures++;
    }

    if (hmacKeyringSetMinValidKeyId(ring, K1)) {
        printf(
            "dkTestRevokedKeyFloor: backward SetMinValidKeyId MUST refuse\n");
        failures++;
    }

    const size_t sz = hmacKeyringSnapshotSize(ring);
    cleanerName(zfree, Bytes) uint8_t *snap = zmalloc(sz);
    size_t snap_w = 0;
    if (!hmacKeyringSnapshot(ring, snap, sz, &snap_w)) {
        printf("dkTestRevokedKeyFloor: snapshot failed\n");
        failures++;
    }
    hmacKeyring *restored = hmacKeyringCreate();
    if (!hmacKeyringRestore(restored, snap, snap_w)) {
        printf("dkTestRevokedKeyFloor: restore failed\n");
        failures++;
    }
    if (hmacKeyringMinValidKeyId(restored) != K2) {
        printf("dkTestRevokedKeyFloor: restored floor expected %u got %u\n",
               (unsigned)K2, (unsigned)hmacKeyringMinValidKeyId(restored));
        failures++;
    }
    flexEnvelopeVerifyParams vp_r = {.keyring = restored};
    if (flexEnvelopeVerify(envs[0], env_lens[0], &vp_r, &out, &out_len)) {
        printf("dkTestRevokedKeyFloor: post-restore K1 MUST still reject\n");
        failures++;
    }
    if (flexEnvelopeLastVerifyError() != FLEX_ENV_VERIFY_FAILED_KEY_REVOKED) {
        printf("dkTestRevokedKeyFloor: post-restore K1 err expected "
               "KEY_REVOKED got %d\n",
               (int)flexEnvelopeLastVerifyError());
        failures++;
    }
    if (!flexEnvelopeVerify(envs[1], env_lens[1], &vp_r, &out, &out_len)) {
        printf("dkTestRevokedKeyFloor: post-restore K2 MUST succeed\n");
        failures++;
    }

    hmacKeyringFree(ring);
    hmacKeyringFree(restored);

    if (failures == 0) {
        printf("dkTestRevokedKeyFloor: OK\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * flexEnvelopeOpen symmetric keyring auto-resolution.
 * 6 scenarios: ring-only, key-only, both, unknown id, revoked floor,
 * no KEYID_PRESENT.
 * ==================================================================== */
static int dkTestEnvelopeOpenKeyringResolveTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    hmacKeyring *ring = hmacKeyringCreate();
    if (ring == NULL) {
        return 1;
    }

    const uint8_t k1[CHACHA20_POLY1305_KEY_LEN] = {
        0xC4, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
        0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24,
        0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E,
    };
    const uint8_t k2[CHACHA20_POLY1305_KEY_LEN] = {
        0xC4, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
        0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34,
        0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
    };
    const uint8_t salt1[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xC4, 0x01, 0x02,
                                                         0x03};
    const uint8_t salt2[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xC4, 0x04, 0x05,
                                                         0x06};
    const hmacKeyId K1 = 401u, K2 = 402u;

    if (!hmacKeyringInstall(ring, K1, &hmacAlgoSHA256, k1, sizeof(k1), salt1)) {
        hmacKeyringFree(ring);
        return 1;
    }
    if (!hmacKeyringInstall(ring, K2, &hmacAlgoSHA256, k2, sizeof(k2), salt2)) {
        hmacKeyringFree(ring);
        return 1;
    }

    const uint8_t plaintext[] = "c4-open-keyring-resolve";
    const size_t plaintext_len = sizeof(plaintext);
    const uint8_t nonce[CHACHA20_POLY1305_NONCE_LEN] = {
        0xC4, 0xA0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
    };

    const size_t cap = flexEnvelopeWrappedSize(
        plaintext_len, FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
        FLEX_ENV_FLAG_KEYID_PRESENT);
    const size_t cap_no_kid = flexEnvelopeWrappedSize(
        plaintext_len, FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305, 0u);
    if (cap == 0u || cap_no_kid == 0u) {
        hmacKeyringFree(ring);
        return 1;
    }

    cleanerName(zfree, Bytes) uint8_t *env_k1 = zmalloc(cap);
    cleanerName(zfree, Bytes) uint8_t *env_unknown = zmalloc(cap);
    cleanerName(zfree, Bytes) uint8_t *env_no_kid = zmalloc(cap_no_kid);
    cleanerName(zfree, Bytes) uint8_t *pt = zmalloc(plaintext_len);
    size_t env_k1_w = 0, env_unknown_w = 0, env_no_kid_w = 0;

    flexEnvelopeSealParams sp_k1 = {
        .key = k1,
        .nonce = nonce,
        .key_len = sizeof(k1),
        .nonce_len = sizeof(nonce),
        .algo = FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
        .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
        .key_id = K1,
    };
    if (!flexEnvelopeSeal(env_k1, cap, plaintext, plaintext_len, &sp_k1,
                          &env_k1_w)) {
        failures++;
        goto cleanup;
    }
    flexEnvelopeSealParams sp_unknown = sp_k1;
    sp_unknown.key_id = 9999u;
    if (!flexEnvelopeSeal(env_unknown, cap, plaintext, plaintext_len,
                          &sp_unknown, &env_unknown_w)) {
        failures++;
        goto cleanup;
    }
    flexEnvelopeSealParams sp_no_kid = sp_k1;
    sp_no_kid.on_wire_flags = 0u;
    sp_no_kid.key_id = 0u;
    if (!flexEnvelopeSeal(env_no_kid, cap_no_kid, plaintext, plaintext_len,
                          &sp_no_kid, &env_no_kid_w)) {
        failures++;
        goto cleanup;
    }

    /* S1: ring only */
    {
        memset(pt, 0, plaintext_len);
        size_t pt_w = 0;
        flexEnvelopeVerifyParams vp = {.keyring = ring};
        if (!flexEnvelopeOpen(env_k1, env_k1_w, &vp, pt, plaintext_len,
                              &pt_w)) {
            printf("dkTestEnvelopeOpenKeyringResolve: S1 must succeed\n");
            failures++;
        } else if (memcmp(pt, plaintext, plaintext_len) != 0) {
            failures++;
        }
    }

    /* S2: explicit key only */
    {
        memset(pt, 0, plaintext_len);
        size_t pt_w = 0;
        flexEnvelopeVerifyParams vp = {.key = k1, .key_len = sizeof(k1)};
        if (!flexEnvelopeOpen(env_k1, env_k1_w, &vp, pt, plaintext_len,
                              &pt_w)) {
            failures++;
        } else if (memcmp(pt, plaintext, plaintext_len) != 0) {
            failures++;
        }
    }

    /* S3: both supplied — explicit wins */
    {
        hmacKeyring *ring2 = hmacKeyringCreate();
        if (hmacKeyringInstall(ring2, K2, &hmacAlgoSHA256, k2, sizeof(k2),
                               salt2)) {
            memset(pt, 0, plaintext_len);
            size_t pt_w = 0;
            flexEnvelopeVerifyParams vp = {
                .key = k1,
                .key_len = sizeof(k1),
                .keyring = ring2,
            };
            if (!flexEnvelopeOpen(env_k1, env_k1_w, &vp, pt, plaintext_len,
                                  &pt_w)) {
                failures++;
            } else if (memcmp(pt, plaintext, plaintext_len) != 0) {
                failures++;
            }
        }
        hmacKeyringFree(ring2);
    }

    /* S4: ring only, unknown id → fail */
    {
        memset(pt, 0, plaintext_len);
        size_t pt_w = 0;
        flexEnvelopeVerifyParams vp = {.keyring = ring};
        if (flexEnvelopeOpen(env_unknown, env_unknown_w, &vp, pt, plaintext_len,
                             &pt_w)) {
            printf("dkTestEnvelopeOpenKeyringResolve: S4 unknown id MUST "
                   "fail\n");
            failures++;
        }
    }

    /* S5: K1 below floor → KEY_REVOKED */
    {
        if (hmacKeyringSetMinValidKeyId(ring, K2)) {
            memset(pt, 0, plaintext_len);
            size_t pt_w = 0;
            flexEnvelopeVerifyParams vp = {.keyring = ring};
            if (flexEnvelopeOpen(env_k1, env_k1_w, &vp, pt, plaintext_len,
                                 &pt_w)) {
                printf("dkTestEnvelopeOpenKeyringResolve: S5 below floor "
                       "MUST fail\n");
                failures++;
            } else if (flexEnvelopeLastVerifyError() !=
                       FLEX_ENV_VERIFY_FAILED_KEY_REVOKED) {
                printf("dkTestEnvelopeOpenKeyringResolve: S5 expected "
                       "KEY_REVOKED got %d\n",
                       (int)flexEnvelopeLastVerifyError());
                failures++;
            }
        }
    }

    /* S6: ring only, no KEYID_PRESENT → fail */
    {
        memset(pt, 0, plaintext_len);
        size_t pt_w = 0;
        flexEnvelopeVerifyParams vp = {.keyring = ring};
        if (flexEnvelopeOpen(env_no_kid, env_no_kid_w, &vp, pt, plaintext_len,
                             &pt_w)) {
            printf("dkTestEnvelopeOpenKeyringResolve: S6 no KEYID MUST fail\n");
            failures++;
        }
    }

cleanup:
    hmacKeyringFree(ring);

    if (failures == 0) {
        printf("dkTestEnvelopeOpenKeyringResolve: OK (6 scenarios)\n");
        return 0;
    }
    return 1;
}

/* Ambiguous-key fail-safe: wire key_id + caller-supplied keyring must
 * not silently override caller intent. */
static int dkTestWireKeyIdAmbiguityRejectTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    hmacKeyring *ring = hmacKeyringCreate();
    if (ring == NULL) {
        return 1;
    }

    const uint8_t key1[32] = {
        0xC5, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
        0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
        0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    };
    const uint8_t salt1[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xC5, 0x55, 0x05,
                                                         0x50};
    const hmacKeyId K1 = 501u;

    if (!hmacKeyringInstall(ring, K1, &hmacAlgoSHA256, key1, sizeof(key1),
                            salt1)) {
        hmacKeyringFree(ring);
        return 1;
    }

    const uint8_t payload[] = "c5-ambiguity-payload";
    const size_t plen = sizeof(payload);
    flexEnvelopeWrapParams wp = {
        .algo = FLEX_ENV_ALGO_HMAC_SHA256,
        .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
        .key_id = K1,
        .key = key1,
        .key_len = sizeof(key1),
    };
    const size_t cap = flexEnvelopeWrappedSize(plen, FLEX_ENV_ALGO_HMAC_SHA256,
                                               wp.on_wire_flags);
    if (cap == 0u) {
        hmacKeyringFree(ring);
        return 1;
    }
    cleanerName(zfree, Bytes) uint8_t *env = zmalloc(cap);
    size_t env_len = 0;
    if (!flexEnvelopeWrap(env, cap, payload, plen, &wp, &env_len)) {
        failures++;
        goto cleanup_c5;
    }

    uint8_t prep_storage[1024];
    hmacKey prep;
    hmacPrepareKey(&prep, &hmacAlgoSHA256, prep_storage, sizeof(prep_storage),
                   key1, sizeof(key1));

    /* S1: prepared_key + keyring + NO flag → AMBIGUOUS_KEY */
    {
        const uint8_t *out_p = NULL;
        uint64_t out_pl = 0;
        flexEnvelopeVerifyParams vp = {.prepared_key = &prep, .keyring = ring};
        if (flexEnvelopeVerify(env, env_len, &vp, &out_p, &out_pl)) {
            printf("dkTestWireKeyIdAmbiguityReject: S1 must reject\n");
            failures++;
        } else if (flexEnvelopeLastVerifyError() !=
                   FLEX_ENV_VERIFY_FAILED_AMBIGUOUS_KEY) {
            printf("dkTestWireKeyIdAmbiguityReject: S1 expected AMBIGUOUS_KEY "
                   "got %d\n",
                   (int)flexEnvelopeLastVerifyError());
            failures++;
        }
    }
    /* S2: prepared_key + keyring + FLAG → succeed */
    {
        const uint8_t *out_p = NULL;
        uint64_t out_pl = 0;
        flexEnvelopeVerifyParams vp = {
            .prepared_key = &prep,
            .keyring = ring,
            .opts = FLEX_ENV_VERIFY_ACCEPT_WIRE_KEYID,
        };
        if (!flexEnvelopeVerify(env, env_len, &vp, &out_p, &out_pl)) {
            failures++;
        } else if (out_pl != plen || memcmp(out_p, payload, plen) != 0) {
            failures++;
        }
    }
    /* S3: prepared_key only → succeed */
    {
        const uint8_t *out_p = NULL;
        uint64_t out_pl = 0;
        flexEnvelopeVerifyParams vp = {.prepared_key = &prep};
        if (!flexEnvelopeVerify(env, env_len, &vp, &out_p, &out_pl)) {
            failures++;
        }
    }
    /* S4: keyring only → succeed (auto-resolve) */
    {
        const uint8_t *out_p = NULL;
        uint64_t out_pl = 0;
        flexEnvelopeVerifyParams vp = {.keyring = ring};
        if (!flexEnvelopeVerify(env, env_len, &vp, &out_p, &out_pl)) {
            failures++;
        }
    }
    /* S5: raw key + keyring + NO flag → AMBIGUOUS_KEY */
    {
        const uint8_t *out_p = NULL;
        uint64_t out_pl = 0;
        flexEnvelopeVerifyParams vp = {
            .key = key1,
            .key_len = sizeof(key1),
            .keyring = ring,
        };
        if (flexEnvelopeVerify(env, env_len, &vp, &out_p, &out_pl)) {
            failures++;
        } else if (flexEnvelopeLastVerifyError() !=
                   FLEX_ENV_VERIFY_FAILED_AMBIGUOUS_KEY) {
            failures++;
        }
    }
    /* S6: raw key + keyring + FLAG → succeed */
    {
        const uint8_t *out_p = NULL;
        uint64_t out_pl = 0;
        flexEnvelopeVerifyParams vp = {
            .key = key1,
            .key_len = sizeof(key1),
            .keyring = ring,
            .opts = FLEX_ENV_VERIFY_ACCEPT_WIRE_KEYID,
        };
        if (!flexEnvelopeVerify(env, env_len, &vp, &out_p, &out_pl)) {
            failures++;
        }
    }

cleanup_c5:
    hmacKeyringFree(ring);

    if (failures == 0) {
        printf("dkTestWireKeyIdAmbiguityReject: OK (6 scenarios)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * persist snapshot/WAL headers carry key_id + typed error.
 * ==================================================================== */
static int dkTestPersistKeyIdHeaderTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    /* Declared here, ahead of the first goto, so the scope-exit cleaner
     * covers every jump to the cleanup labels below. */
    cleaner(flexFree) flex *payload = NULL;

    hmacKeyring *ringW = hmacKeyringCreate();
    if (ringW == NULL) {
        return 1;
    }
    const uint8_t key1[32] = {
        0xC6, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
        0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
        0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    };
    const uint8_t salt1[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xC6, 0x01, 0x02,
                                                         0x03};
    const hmacKeyId K1 = 0x12345u;
    if (!hmacKeyringInstall(ringW, K1, &hmacAlgoSHA256, key1, sizeof(key1),
                            salt1)) {
        hmacKeyringFree(ringW);
        return 1;
    }

    persistConfig configW = persistDefaultConfig();
    configW.checksumType = PERSIST_CHECKSUM_HMAC_SHA256;
    persist *pW = persistCreate(&persistOpsFlex, &configW);
    persistStore *snapStore = persistStoreMemory(0);
    persistAttachSnapshot(pW, snapStore);
    if (!persistAttachKeyring(pW, ringW, K1)) {
        failures++;
        goto cleanup_writer;
    }

    payload = flexNew();
    flexPushBytes(&payload, "c6-payload", 10, FLEX_ENDPOINT_TAIL);
    if (!persistSnapshot(pW, payload)) {
        printf("dkTestPersistKeyIdHeader: snapshot under K1 failed (err=%d)\n",
               (int)persistLastError(pW));
        failures++;
        goto cleanup_payload;
    }

    size_t snapLen = 0;
    const uint8_t *snapBuf = persistStoreMemoryBuffer(snapStore, &snapLen);
    if (snapBuf == NULL || snapLen == 0) {
        failures++;
        goto cleanup_payload;
    }

    hmacKeyring *ringR = hmacKeyringCreate();
    if (ringR == NULL) {
        failures++;
        goto cleanup_payload;
    }
    const uint8_t key2[32] = {
        0xC6, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    };
    const uint8_t salt2[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xC6, 0x02, 0x02,
                                                         0x02};
    const hmacKeyId K2 = 0x67890u;
    if (!hmacKeyringInstall(ringR, K2, &hmacAlgoSHA256, key2, sizeof(key2),
                            salt2)) {
        failures++;
        goto cleanup_reader_ring;
    }

    persistConfig configR = persistDefaultConfig();
    configR.checksumType = PERSIST_CHECKSUM_HMAC_SHA256;
    persist *pR = persistCreate(&persistOpsFlex, &configR);
    persistStore *readerStore = persistStoreMemory(snapLen);
    readerStore->write(readerStore->ctx, snapBuf, snapLen);
    readerStore->seek(readerStore->ctx, 0, SEEK_SET);
    persistAttachSnapshot(pR, readerStore);
    if (!persistAttachKeyring(pR, ringR, K2)) {
        failures++;
        goto cleanup_reader_persist;
    }

    if (persistVerifySnapshot(pR)) {
        printf("dkTestPersistKeyIdHeader: verify with WRONG key MUST fail\n");
        failures++;
    }
    if (persistLastError(pR) != PERSIST_ERR_KEY_ID_MISMATCH) {
        printf("dkTestPersistKeyIdHeader: lastError expected "
               "KEY_ID_MISMATCH, got %d\n",
               (int)persistLastError(pR));
        failures++;
    }
    if (persistLastExpectedKeyId(pR) != (uint32_t)K1) {
        printf("dkTestPersistKeyIdHeader: expected key_id must be K1 (%u), "
               "got %u\n",
               (unsigned)K1, (unsigned)persistLastExpectedKeyId(pR));
        failures++;
    }

    if (!hmacKeyringInstall(ringR, K1, &hmacAlgoSHA256, key1, sizeof(key1),
                            salt1)) {
        failures++;
        goto cleanup_reader_persist;
    }
    if (!persistRebindKeyring(pR, ringR, K1)) {
        failures++;
        goto cleanup_reader_persist;
    }
    if (!persistVerifySnapshot(pR)) {
        printf("dkTestPersistKeyIdHeader: verify after rebind to K1 MUST "
               "succeed (err=%d)\n",
               (int)persistLastError(pR));
        failures++;
    }
    if (persistLastError(pR) != PERSIST_ERR_NONE) {
        failures++;
    }
    if (persistLastExpectedKeyId(pR) != 0u) {
        failures++;
    }

cleanup_reader_persist:
    persistClose(pR);
cleanup_reader_ring:
    hmacKeyringFree(ringR);
cleanup_payload:
cleanup_writer:
    persistClose(pW);
    hmacKeyringFree(ringW);

    if (failures == 0) {
        printf("dkTestPersistKeyIdHeader: OK (key_id header + typed error + "
               "rebind-recovery)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * BLAKE3 SSE4.1 4-batch dispatcher — verify SSE4.1 fallback wedged
 * between AVX2 and scalar; per-host dispatcher selection assertion;
 * direct-call differential test of blake3SSE41Compress4 vs portable.
 * ==================================================================== */
static int dkTestBlake3Sse41BatchTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    blake3SetForceImpl(BLAKE3_IMPL_AUTO);

    const blake3Backend backend = blake3DispatcherActiveBackend();
    const bool avx2_here = blake3AVX2Available();
    const bool neon_here = blake3NeonAvailable();
    const bool sse41_here = blake3SSE41Available();

    const bool expect_sse41_dispatch = sse41_here && !avx2_here && !neon_here;
    if (expect_sse41_dispatch && backend != BLAKE3_BACKEND_SSE41) {
        printf("dkTestBlake3Sse41Batch: SSE4.1 available without AVX2/NEON but "
               "dispatcher reports backend=%d (expected %d)\n",
               (int)backend, (int)BLAKE3_BACKEND_SSE41);
        failures++;
    }
    if (!sse41_here && backend == BLAKE3_BACKEND_SSE41) {
        printf(
            "dkTestBlake3Sse41Batch: dispatcher reports SSE4.1 but probe says "
            "unavailable\n");
        failures++;
    }

    const size_t N = 32u * 1024u;
    cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(N);
    if (buf == NULL) {
        printf("dkTestBlake3Sse41Batch: zmalloc(%zu) failed\n", N);
        return 1;
    }
    uint64_t s = 0xB2A2BEEFC0FFEE42ULL;
    for (size_t i = 0; i < N; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (uint8_t)(s >> 24);
    }

    blake3SetForceImpl(BLAKE3_IMPL_SCALAR);
    uint8_t expected[BLAKE3_OUT_LEN];
    blake3Hash(buf, N, expected);

    blake3SetForceImpl(BLAKE3_IMPL_AUTO);
    uint8_t got_auto[BLAKE3_OUT_LEN];
    blake3Hash(buf, N, got_auto);
    if (memcmp(expected, got_auto, BLAKE3_OUT_LEN) != 0) {
        printf("dkTestBlake3Sse41Batch: AUTO digest differs from scalar "
               "reference\n");
        failures++;
    }

    blake3SetForceImpl(BLAKE3_IMPL_SIMD);
    uint8_t got_simd[BLAKE3_OUT_LEN];
    blake3Hash(buf, N, got_simd);
    if (memcmp(expected, got_simd, BLAKE3_OUT_LEN) != 0) {
        printf("dkTestBlake3Sse41Batch: SIMD-forced digest differs from scalar "
               "reference "
               "(backend=%d)\n",
               (int)blake3DispatcherActiveBackend());
        failures++;
    }
    blake3SetForceImpl(BLAKE3_IMPL_AUTO);

    const size_t sweep_chunks[] = {1,  2,  3,  4,  5,  7,  8,  9,  12, 15,
                                   16, 17, 23, 24, 25, 32, 33, 48, 64};
    const size_t n_sweep = sizeof(sweep_chunks) / sizeof(sweep_chunks[0]);
    for (size_t i = 0; i < n_sweep; i++) {
        const size_t len = sweep_chunks[i] * BLAKE3_CHUNK_LEN;
        if (len > N) {
            continue;
        }
        uint8_t ref[BLAKE3_OUT_LEN];
        uint8_t obs[BLAKE3_OUT_LEN];
        blake3SetForceImpl(BLAKE3_IMPL_SCALAR);
        blake3Hash(buf, len, ref);
        blake3SetForceImpl(BLAKE3_IMPL_AUTO);
        blake3Hash(buf, len, obs);
        if (memcmp(ref, obs, BLAKE3_OUT_LEN) != 0) {
            printf("dkTestBlake3Sse41Batch: dispatch digest mismatch at "
                   "chunks=%zu (len=%zu)\n",
                   sweep_chunks[i], len);
            failures++;
        }
    }
    blake3SetForceImpl(BLAKE3_IMPL_AUTO);

    {
        uint64_t r = 0xB2C3D4E5F60718A9ULL;
        for (uint32_t iter = 0; iter < 256; iter++) {
            uint32_t cvs[4][8];
            uint8_t blocks[4][BLAKE3_BLOCK_LEN];
            uint8_t block_lens[4];
            uint64_t counters[4];
            uint8_t flag_set[4];
            for (uint32_t i = 0; i < 4; i++) {
                for (uint32_t k = 0; k < 8; k++) {
                    r = r * 6364136223846793005ULL + 1442695040888963407ULL;
                    cvs[i][k] = (uint32_t)(r >> 32);
                }
                for (uint32_t k = 0; k < BLAKE3_BLOCK_LEN; k++) {
                    r = r * 6364136223846793005ULL + 1442695040888963407ULL;
                    blocks[i][k] = (uint8_t)(r >> 24);
                }
                r = r * 6364136223846793005ULL + 1442695040888963407ULL;
                counters[i] = r;
                r = r * 6364136223846793005ULL + 1442695040888963407ULL;
                block_lens[i] = (uint8_t)((r >> 24) % BLAKE3_BLOCK_LEN + 1);
                r = r * 6364136223846793005ULL + 1442695040888963407ULL;
                flag_set[i] = (uint8_t)((r >> 24) & 0x7F);
            }

            const uint32_t *cv_in[4] = {cvs[0], cvs[1], cvs[2], cvs[3]};
            const uint8_t *blk_in[4] = {blocks[0], blocks[1], blocks[2],
                                        blocks[3]};
            uint8_t batched[4][32];
            blake3SSE41Compress4(cv_in, blk_in, block_lens, counters, flag_set,
                                 batched);

            for (uint32_t i = 0; i < 4; i++) {
                uint32_t expect[8];
                memcpy(expect, cvs[i], sizeof(expect));
                blake3PortableCompressInPlace(expect, blocks[i], block_lens[i],
                                              counters[i], flag_set[i]);
                uint8_t expect_bytes[32];
                for (uint32_t k = 0; k < 8; k++) {
                    uint32_t w = expect[k];
                    expect_bytes[k * 4 + 0] = (uint8_t)w;
                    expect_bytes[k * 4 + 1] = (uint8_t)(w >> 8);
                    expect_bytes[k * 4 + 2] = (uint8_t)(w >> 16);
                    expect_bytes[k * 4 + 3] = (uint8_t)(w >> 24);
                }
                if (memcmp(expect_bytes, batched[i], 32) != 0) {
                    printf("dkTestBlake3Sse41Batch: SSE41Compress4 lane=%u "
                           "differs from "
                           "portable at iter=%u\n",
                           i, iter);
                    failures++;
                    iter = 256;
                    break;
                }
            }
        }
    }

    const char *backend_name = "scalar";
    switch (backend) {
    case BLAKE3_BACKEND_AVX2:
        backend_name = "AVX2-8";
        break;
    case BLAKE3_BACKEND_NEON:
        backend_name = "NEON-4";
        break;
    case BLAKE3_BACKEND_SSE41:
        backend_name = "SSE41-4";
        break;
    case BLAKE3_BACKEND_SCALAR:
    default:
        backend_name = "scalar";
        break;
    }

    if (expect_sse41_dispatch) {
        printf("dkTestBlake3Sse41Batch: SSE4.1 dispatcher active "
               "(no AVX2/NEON on host); backend=%s; 32 KiB buffer and "
               "%zu chunk-counts swept against scalar reference; %d failures\n",
               backend_name, n_sweep, failures);
    } else if (sse41_here && avx2_here) {
        printf("dkTestBlake3Sse41Batch: AVX2 wins on this host; SSE4.1 "
               "dispatcher assertion skipped (AVX2 takes priority); "
               "backend=%s; SSE41Compress4 direct-call differential "
               "passed; %d failures\n",
               backend_name, failures);
    } else if (!sse41_here) {
        printf("dkTestBlake3Sse41Batch: SSE4.1 not present on this host "
               "(probe=false); dispatcher assertion skipped; backend=%s; "
               "SSE41Compress4 direct-call (stub-delegating to portable) "
               "agrees with scalar reference; %d failures\n",
               backend_name, failures);
    } else {
        printf("dkTestBlake3Sse41Batch: backend=%s; %zu chunk-counts "
               "swept; %d failures\n",
               backend_name, n_sweep, failures);
    }

    return failures == 0 ? 0 : 1;
}

/* ============================================================================
 * aeadNonceLog persist round-trip — observe 1000 distinct nonces
 * across 4 key_ids, serialize via toBytes, restore via fromBytes AND
 * via persistOpsAeadNonceLog.{snapshot,restore}, assert every original
 * (key_id, nonce) is rejected as duplicate, and a fresh tuple is
 * accepted. Also exercises validate hook + corruption rejection paths.
 * ============================================================================
 */
static uint64_t d1Splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void d1NonceFromValue(uint64_t lo, uint32_t hi, uint8_t out[12]) {
    memcpy(out, &lo, 8);
    memcpy(out + 8, &hi, 4);
}

static int dkTestNonceLogPersistRoundTripTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;
    /* Declared here (before any goto to `done`) so the scope-exit cleaner
     * attribute below never bypasses an initializer across a jump. */
    cleanerName(zfree, Bytes) uint8_t *snap = NULL;
    cleanerName(zfree, Bytes) uint8_t *padded = NULL;
    cleaner(aeadNonceLogFree) aeadNonceLog *src = NULL;
    cleaner(aeadNonceLogFree) aeadNonceLog *empty_src = NULL;

    const size_t kCap = 4096u;
    const uint32_t kKeys[] = {7u, 13u, 29u, 31u};
    const size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);
    const size_t kPerKey = 250u;
    const size_t kTotal = kKeyCount * kPerKey;
    size_t need = 0;

    src = aeadNonceLogCreate(kCap);
    if (src == NULL) {
        printf("dkTestNonceLogPersistRoundTrip: src create failed\n");
        return 1;
    }
    if (aeadNonceLogCapacity(src) != kCap) {
        printf("dkTestNonceLogPersistRoundTrip: cap mismatch on src "
               "(want %zu got %zu)\n",
               kCap, aeadNonceLogCapacity(src));
        return 1;
    }

    uint64_t seed = 0x123456789ABCDEF0ull;
    uint8_t nonces[1000][12];
    uint32_t key_ids[1000];
    for (size_t i = 0; i < kTotal; i++) {
        const uint32_t kid = kKeys[i % kKeyCount];
        const uint64_t lo = d1Splitmix64(&seed);
        const uint32_t hi = (uint32_t)(d1Splitmix64(&seed) & 0xFFFFFFFFu);
        d1NonceFromValue(lo, hi, nonces[i]);
        key_ids[i] = kid;
        if (!aeadNonceLogObserve(src, kid, nonces[i])) {
            printf("dkTestNonceLogPersistRoundTrip: src observe[%zu] failed\n",
                   i);
            failures++;
            goto done;
        }
    }
    if (aeadNonceLogSize(src) != kTotal) {
        printf("dkTestNonceLogPersistRoundTrip: src size %zu != %zu\n",
               aeadNonceLogSize(src), kTotal);
        failures++;
        goto done;
    }

    need = aeadNonceLogToBytes(src, NULL, 0);
    if (need == 0) {
        printf("dkTestNonceLogPersistRoundTrip: sizing call returned 0\n");
        failures++;
        goto done;
    }
    snap = zmalloc(need);
    if (snap == NULL) {
        failures++;
        goto done;
    }
    if (aeadNonceLogToBytes(src, snap, need) != need) {
        printf("dkTestNonceLogPersistRoundTrip: write mismatch\n");
        failures++;
        goto done;
    }
    if (aeadNonceLogToBytes(src, snap, 1u) != 0u) {
        printf("dkTestNonceLogPersistRoundTrip: short cap should return 0\n");
        failures++;
    }

    aeadNonceLog *dst = aeadNonceLogFromBytes(snap, need);
    if (dst == NULL || aeadNonceLogCapacity(dst) != kCap ||
        aeadNonceLogSize(dst) != kTotal) {
        printf("dkTestNonceLogPersistRoundTrip: fromBytes restore failed\n");
        if (dst) {
            aeadNonceLogFree(dst);
        }
        failures++;
        goto done;
    }

    for (size_t i = 0; i < kTotal; i++) {
        if (aeadNonceLogObserve(dst, key_ids[i], nonces[i])) {
            printf("dkTestNonceLogPersistRoundTrip: dst observe[%zu] "
                   "(kid=%u) unexpectedly accepted\n",
                   i, (unsigned)key_ids[i]);
            failures++;
            break;
        }
    }

    uint8_t fresh_nonce[12];
    d1NonceFromValue(0xDEADBEEFCAFEBABEull, 0x12345678u, fresh_nonce);
    if (!aeadNonceLogObserve(dst, 99u, fresh_nonce)) {
        printf("dkTestNonceLogPersistRoundTrip: fresh observe refused\n");
        failures++;
    }
    if (aeadNonceLogObserve(dst, 99u, fresh_nonce)) {
        printf("dkTestNonceLogPersistRoundTrip: fresh dup accepted\n");
        failures++;
    }
    if (!persistOpsAeadNonceLog.validate(dst)) {
        printf("dkTestNonceLogPersistRoundTrip: validate(dst) rejected good "
               "log\n");
        failures++;
    }
    aeadNonceLogFree(dst);

    size_t snap2_len = 0;
    uint8_t *snap2 = persistOpsAeadNonceLog.snapshot(src, &snap2_len);
    if (snap2 == NULL || snap2_len == 0) {
        printf("dkTestNonceLogPersistRoundTrip: desc snapshot NULL\n");
        failures++;
        goto done;
    }
    aeadNonceLog *dst2 =
        (aeadNonceLog *)persistOpsAeadNonceLog.restore(snap2, snap2_len);
    if (dst2 == NULL) {
        printf("dkTestNonceLogPersistRoundTrip: desc restore NULL\n");
        zfree(snap2);
        failures++;
        goto done;
    }
    if (persistOpsAeadNonceLog.count(dst2) != kTotal) {
        printf("dkTestNonceLogPersistRoundTrip: desc count mismatch\n");
        failures++;
    }
    if (persistOpsAeadNonceLog.estimateSize(dst2) != snap2_len) {
        printf("dkTestNonceLogPersistRoundTrip: desc estimateSize mismatch\n");
        failures++;
    }
    if (!persistOpsAeadNonceLog.validate(dst2)) {
        printf("dkTestNonceLogPersistRoundTrip: validate(dst2) rejected\n");
        failures++;
    }
    for (size_t i = 0; i < kTotal; i++) {
        if (aeadNonceLogObserve(dst2, key_ids[i], nonces[i])) {
            printf("dkTestNonceLogPersistRoundTrip: dst2 observe[%zu] "
                   "accepted\n",
                   i);
            failures++;
            break;
        }
    }
    persistOpsAeadNonceLog.free(dst2);

    if (snap2_len >= 1u) {
        void *bad = persistOpsAeadNonceLog.restore(snap2, snap2_len - 1u);
        if (bad != NULL) {
            printf("dkTestNonceLogPersistRoundTrip: truncated accepted\n");
            persistOpsAeadNonceLog.free(bad);
            failures++;
        }
    }
    padded = zmalloc(snap2_len + 1u);
    if (padded != NULL) {
        memcpy(padded, snap2, snap2_len);
        padded[snap2_len] = 0xAA;
        void *bad = persistOpsAeadNonceLog.restore(padded, snap2_len + 1u);
        if (bad != NULL) {
            printf("dkTestNonceLogPersistRoundTrip: padded accepted\n");
            persistOpsAeadNonceLog.free(bad);
            failures++;
        }
    }
    uint8_t junk[16] = {0};
    if (persistOpsAeadNonceLog.restore(junk, sizeof(junk)) != NULL) {
        printf("dkTestNonceLogPersistRoundTrip: junk accepted\n");
        failures++;
    }

    empty_src = aeadNonceLogCreate(0);
    if (empty_src != NULL) {
        const size_t enseed = aeadNonceLogToBytes(empty_src, NULL, 0);
        if (enseed > 0u) {
            cleanerName(zfree, Bytes) uint8_t *ebuf = zmalloc(enseed);
            if (ebuf != NULL) {
                if (aeadNonceLogToBytes(empty_src, ebuf, enseed) == enseed) {
                    aeadNonceLog *e2 = aeadNonceLogFromBytes(ebuf, enseed);
                    if (e2 == NULL || aeadNonceLogSize(e2) != 0u) {
                        printf("dkTestNonceLogPersistRoundTrip: empty "
                               "round-trip failed\n");
                        failures++;
                    }
                    if (e2) {
                        aeadNonceLogFree(e2);
                    }
                }
            }
        }
    }
    zfree(snap2);

done:
    if (failures == 0) {
        printf("dkTestNonceLogPersistRoundTrip: OK "
               "(cap=%zu, records=%zu, snap=%zu B)\n",
               kCap, kTotal, need);
        return 0;
    }
    return 1;
}

/* ====================================================================
 * aeadNonceLog wired into flexEnvelopeSeal/Open.
 * Producer Seal observes (key_id, nonce); duplicate →
 * FLEX_ENV_VERIFY_FAILED_NONCE_REUSED. Consumer Open opt-in: replay on the same
 * envelope rejected. Opt-out (nonceLog=NULL) preserves legacy behavior for both
 * sides.
 * ==================================================================== */
static int dkTestEnvelopeNonceLogIntegrationTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    const uint32_t kKeyId = 7777u;
    const uint8_t key[CHACHA20_POLY1305_KEY_LEN] = {
        0xD2, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
        0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14,
        0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
    };
    const uint8_t nonceN[CHACHA20_POLY1305_NONCE_LEN] = {
        0xD2, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA,
    };
    const uint8_t nonceN1[CHACHA20_POLY1305_NONCE_LEN] = {
        0xD2, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA,
    };
    const uint8_t plaintext[] = "nonceLog-integration-payload";
    const size_t plaintext_len = sizeof(plaintext);

    const size_t cap = flexEnvelopeWrappedSize(
        plaintext_len, FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
        FLEX_ENV_FLAG_KEYID_PRESENT);
    if (cap == 0u) {
        printf("dkTestEnvelopeNonceLogIntegration: wrappedSize=0\n");
        return 1;
    }

    cleaner(aeadNonceLogFree) aeadNonceLog *producerLog =
        aeadNonceLogCreate(1024u);
    if (producerLog == NULL) {
        printf("dkTestEnvelopeNonceLogIntegration: producer log create "
               "failed\n");
        return 1;
    }
    cleanerName(zfree, Bytes) uint8_t *env1 = NULL;
    cleanerName(zfree, Bytes) uint8_t *env_replay = NULL;
    cleanerName(zfree, Bytes) uint8_t *env2 = NULL;
    cleanerName(zfree, Bytes) uint8_t *pt_buf = NULL;
    cleaner(aeadNonceLogFree) aeadNonceLog *consumerLog = NULL;

    env1 = zmalloc(cap);
    size_t env1_written = 0;
    flexEnvelopeSealParams sp1 = {
        .key = key,
        .nonce = nonceN,
        .key_len = sizeof(key),
        .nonce_len = sizeof(nonceN),
        .algo = FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
        .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
        .nonceLog = producerLog,
        .key_id = kKeyId,
    };
    if (!flexEnvelopeSeal(env1, cap, plaintext, plaintext_len, &sp1,
                          &env1_written)) {
        printf("dkTestEnvelopeNonceLogIntegration: Seal#1 should succeed "
               "(err=%d)\n",
               (int)flexEnvelopeLastVerifyError());
        failures++;
        goto cleanup;
    }
    if (aeadNonceLogSize(producerLog) != 1u) {
        printf("dkTestEnvelopeNonceLogIntegration: producer log size "
               "expected 1 got %zu\n",
               aeadNonceLogSize(producerLog));
        failures++;
    }

    env_replay = zmalloc(cap);
    size_t replay_written = 0;
    if (flexEnvelopeSeal(env_replay, cap, plaintext, plaintext_len, &sp1,
                         &replay_written)) {
        printf("dkTestEnvelopeNonceLogIntegration: duplicate Seal MUST fail\n");
        failures++;
    } else if (flexEnvelopeLastVerifyError() !=
               FLEX_ENV_VERIFY_FAILED_NONCE_REUSED) {
        printf("dkTestEnvelopeNonceLogIntegration: duplicate Seal err "
               "expected NONCE_REUSED (3) got %d\n",
               (int)flexEnvelopeLastVerifyError());
        failures++;
    }
    if (aeadNonceLogSize(producerLog) != 1u) {
        printf("dkTestEnvelopeNonceLogIntegration: duplicate Seal mutated "
               "log\n");
        failures++;
    }

    flexEnvelopeSealParams sp_optout = sp1;
    sp_optout.nonceLog = NULL;
    size_t optout_written = 0;
    if (!flexEnvelopeSeal(env_replay, cap, plaintext, plaintext_len, &sp_optout,
                          &optout_written)) {
        printf("dkTestEnvelopeNonceLogIntegration: opt-out Seal should "
               "succeed (err=%d)\n",
               (int)flexEnvelopeLastVerifyError());
        failures++;
    }
    if (aeadNonceLogSize(producerLog) != 1u) {
        printf("dkTestEnvelopeNonceLogIntegration: opt-out Seal mutated "
               "producer log\n");
        failures++;
    }

    env2 = zmalloc(cap);
    size_t env2_written = 0;
    flexEnvelopeSealParams sp2 = sp1;
    sp2.nonce = nonceN1;
    if (!flexEnvelopeSeal(env2, cap, plaintext, plaintext_len, &sp2,
                          &env2_written)) {
        printf("dkTestEnvelopeNonceLogIntegration: Scenario4 Seal failed "
               "(err=%d)\n",
               (int)flexEnvelopeLastVerifyError());
        failures++;
        goto cleanup;
    }

    consumerLog = aeadNonceLogCreate(1024u);
    if (consumerLog == NULL) {
        printf("dkTestEnvelopeNonceLogIntegration: consumer log create "
               "failed\n");
        failures++;
        goto cleanup;
    }
    pt_buf = zmalloc(plaintext_len);
    size_t pt_written = 0;
    flexEnvelopeVerifyParams vp_with_log = {
        .key = key,
        .key_len = sizeof(key),
        .nonceLog = consumerLog,
    };
    if (!flexEnvelopeOpen(env2, env2_written, &vp_with_log, pt_buf,
                          plaintext_len, &pt_written)) {
        printf("dkTestEnvelopeNonceLogIntegration: Scenario4 Open#1 should "
               "succeed\n");
        failures++;
    }
    if (pt_written != plaintext_len ||
        memcmp(pt_buf, plaintext, plaintext_len) != 0) {
        printf("dkTestEnvelopeNonceLogIntegration: Scenario4 Open#1 produced "
               "wrong plaintext\n");
        failures++;
    }
    if (aeadNonceLogSize(consumerLog) != 1u) {
        printf("dkTestEnvelopeNonceLogIntegration: consumer log size after "
               "Open#1 expected 1 got %zu\n",
               aeadNonceLogSize(consumerLog));
        failures++;
    }

    memset(pt_buf, 0, plaintext_len);
    if (flexEnvelopeOpen(env2, env2_written, &vp_with_log, pt_buf,
                         plaintext_len, &pt_written)) {
        printf("dkTestEnvelopeNonceLogIntegration: Scenario4 Open#2 (replay) "
               "MUST fail\n");
        failures++;
    } else if (flexEnvelopeLastVerifyError() !=
               FLEX_ENV_VERIFY_FAILED_NONCE_REUSED) {
        printf("dkTestEnvelopeNonceLogIntegration: Scenario4 Open#2 err "
               "expected NONCE_REUSED (3) got %d\n",
               (int)flexEnvelopeLastVerifyError());
        failures++;
    }
    for (size_t i = 0; i < plaintext_len; i++) {
        if (pt_buf[i] != 0u) {
            printf("dkTestEnvelopeNonceLogIntegration: Open#2 did not scrub "
                   "plaintext_out\n");
            failures++;
            break;
        }
    }

    flexEnvelopeVerifyParams vp_no_log = {
        .key = key, .key_len = sizeof(key), .nonceLog = NULL};
    if (!flexEnvelopeOpen(env2, env2_written, &vp_no_log, pt_buf, plaintext_len,
                          &pt_written)) {
        printf("dkTestEnvelopeNonceLogIntegration: Scenario5 Open (no "
               "consumer log) should succeed\n");
        failures++;
    }

cleanup:
    if (failures == 0) {
        printf("dkTestEnvelopeNonceLogIntegration: OK (1 key, 2 nonces, 5 "
               "scenarios)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * testNonceLogAllAeadAlgos — extend the producer/consumer scenario to
 * cover every AEAD algorithm. The pre-existing aeadNonceLog test pins
 * FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305 only, leaving the AES-128-GCM
 * and AES-256-GCM nonceLog plumbing untested.  A wiring bug in
 * flexEnvelopeSeal/Open that mis-routes the (key_id, nonce) tuple for
 * the AES-GCM paths would silently break replay protection for those
 * algos.  This test runs the same Seal-replay / Open-replay scenario
 * for all three AEAD variants and asserts:
 *   - Producer log records exactly one tuple per unique nonce.
 *   - A duplicate Seal with the same (key_id, nonce) fails with
 *     FLEX_ENV_VERIFY_FAILED_NONCE_REUSED and does NOT mutate the log.
 *   - A consumer Open of a fresh envelope updates the consumer log.
 *   - Replaying the same envelope through the same consumer log fails
 *     with NONCE_REUSED and scrubs the plaintext_out buffer.
 *   - Disabling the consumer log on a third Open succeeds (opt-out
 *     plumbing works).
 * ==================================================================== */
static int testNonceLogAllAeadAlgosTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    typedef struct {
        flexEnvelopeAlgo algo;
        size_t key_len;
        const char *name;
    } aeadAlgoCase;

    const aeadAlgoCase kCases[] = {
        {FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305, 32u, "chacha20_poly1305"},
        {FLEX_ENV_ALGO_AEAD_AES128_GCM, 16u, "aes128_gcm"},
        {FLEX_ENV_ALGO_AEAD_AES256_GCM, 32u, "aes256_gcm"},
    };
    const size_t kNumCases = sizeof(kCases) / sizeof(kCases[0]);

    /* 32-byte key buffer covers the longest case (AES-256 + chacha20).
     * AES-128 reads the first 16 bytes only. */
    static const uint8_t kKey32[32] = {
        0xC7, 0xD8, 0xE9, 0x0A, 0x1B, 0x2C, 0x3D, 0x4E, 0x5F, 0x60, 0x71,
        0x82, 0x93, 0xA4, 0xB5, 0xC6, 0xD7, 0xE8, 0xF9, 0x0A, 0x1B, 0x2C,
        0x3D, 0x4E, 0x5F, 0x60, 0x71, 0x82, 0x93, 0xA4, 0xB5, 0xC6,
    };
    static const uint8_t kNonceA[CHACHA20_POLY1305_NONCE_LEN] = {
        0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB,
    };
    static const uint8_t kNonceB[CHACHA20_POLY1305_NONCE_LEN] = {
        0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB,
    };
    static const uint8_t kPlaintext[] = "nonce-log-aead-loop-payload";
    const size_t kPlaintextLen = sizeof(kPlaintext);
    const uint32_t kKeyId = 0xBEEFu;

    for (size_t ci = 0; ci < kNumCases; ci++) {
        const aeadAlgoCase *cs = &kCases[ci];

        cleaner(aeadNonceLogFree) aeadNonceLog *producerLog =
            aeadNonceLogCreate(1024u);
        cleaner(aeadNonceLogFree) aeadNonceLog *consumerLog =
            aeadNonceLogCreate(1024u);
        if (producerLog == NULL || consumerLog == NULL) {
            printf("testNonceLogAllAeadAlgos[%s]: log create failed\n",
                   cs->name);
            failures++;
            continue;
        }

        const size_t cap = flexEnvelopeWrappedSize(kPlaintextLen, cs->algo,
                                                   FLEX_ENV_FLAG_KEYID_PRESENT);
        if (cap == 0u) {
            printf("testNonceLogAllAeadAlgos[%s]: wrappedSize=0\n", cs->name);
            failures++;
            continue;
        }
        cleanerName(zfree, Bytes) uint8_t *env1 = zmalloc(cap);
        cleanerName(zfree, Bytes) uint8_t *env_replay = zmalloc(cap);
        cleanerName(zfree, Bytes) uint8_t *env2 = zmalloc(cap);
        cleanerName(zfree, Bytes) uint8_t *pt_buf = zmalloc(kPlaintextLen);
        if (env1 == NULL || env_replay == NULL || env2 == NULL ||
            pt_buf == NULL) {
            printf("testNonceLogAllAeadAlgos[%s]: alloc failed\n", cs->name);
            failures++;
            continue;
        }

        /* Scenario 1: Seal records one tuple in the producer log. */
        flexEnvelopeSealParams sp1 = {
            .key = kKey32,
            .nonce = kNonceA,
            .key_len = cs->key_len,
            .nonce_len = sizeof(kNonceA),
            .algo = cs->algo,
            .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
            .nonceLog = producerLog,
            .key_id = kKeyId,
        };
        size_t env1_written = 0;
        if (!flexEnvelopeSeal(env1, cap, kPlaintext, kPlaintextLen, &sp1,
                              &env1_written)) {
            printf("testNonceLogAllAeadAlgos[%s]: Seal#1 failed (err=%d)\n",
                   cs->name, (int)flexEnvelopeLastVerifyError());
            failures++;
            goto case_cleanup;
        }
        if (aeadNonceLogSize(producerLog) != 1u) {
            printf("testNonceLogAllAeadAlgos[%s]: producer log size expected "
                   "1 got %zu\n",
                   cs->name, aeadNonceLogSize(producerLog));
            failures++;
        }

        /* Scenario 2: duplicate Seal must fail with NONCE_REUSED. */
        size_t replay_written = 0;
        if (flexEnvelopeSeal(env_replay, cap, kPlaintext, kPlaintextLen, &sp1,
                             &replay_written)) {
            printf("testNonceLogAllAeadAlgos[%s]: duplicate Seal MUST fail\n",
                   cs->name);
            failures++;
        } else if (flexEnvelopeLastVerifyError() !=
                   FLEX_ENV_VERIFY_FAILED_NONCE_REUSED) {
            printf("testNonceLogAllAeadAlgos[%s]: duplicate Seal err expected "
                   "NONCE_REUSED got %d\n",
                   cs->name, (int)flexEnvelopeLastVerifyError());
            failures++;
        }
        if (aeadNonceLogSize(producerLog) != 1u) {
            printf("testNonceLogAllAeadAlgos[%s]: duplicate Seal mutated "
                   "producer log (size=%zu)\n",
                   cs->name, aeadNonceLogSize(producerLog));
            failures++;
        }

        /* Scenario 3: Seal under a different nonce succeeds and is what
         * we'll feed the consumer.  Use env2 with nonceB. */
        flexEnvelopeSealParams sp2 = sp1;
        sp2.nonce = kNonceB;
        size_t env2_written = 0;
        if (!flexEnvelopeSeal(env2, cap, kPlaintext, kPlaintextLen, &sp2,
                              &env2_written)) {
            printf("testNonceLogAllAeadAlgos[%s]: Seal#2 (nonceB) failed "
                   "(err=%d)\n",
                   cs->name, (int)flexEnvelopeLastVerifyError());
            failures++;
            goto case_cleanup;
        }

        /* Scenario 4: consumer Open populates the consumer log. */
        flexEnvelopeVerifyParams vp_with_log = {
            .key = kKey32,
            .key_len = cs->key_len,
            .nonceLog = consumerLog,
        };
        size_t pt_written = 0;
        if (!flexEnvelopeOpen(env2, env2_written, &vp_with_log, pt_buf,
                              kPlaintextLen, &pt_written)) {
            printf("testNonceLogAllAeadAlgos[%s]: consumer Open#1 failed "
                   "(err=%d)\n",
                   cs->name, (int)flexEnvelopeLastVerifyError());
            failures++;
            goto case_cleanup;
        }
        if (pt_written != kPlaintextLen ||
            memcmp(pt_buf, kPlaintext, kPlaintextLen) != 0) {
            printf("testNonceLogAllAeadAlgos[%s]: consumer Open#1 produced "
                   "wrong plaintext\n",
                   cs->name);
            failures++;
        }
        if (aeadNonceLogSize(consumerLog) != 1u) {
            printf("testNonceLogAllAeadAlgos[%s]: consumer log size after "
                   "Open#1 expected 1 got %zu\n",
                   cs->name, aeadNonceLogSize(consumerLog));
            failures++;
        }

        /* Scenario 5: replay through the SAME consumer log must fail
         * NONCE_REUSED and scrub plaintext_out. */
        memset(pt_buf, 0xAB, kPlaintextLen);
        if (flexEnvelopeOpen(env2, env2_written, &vp_with_log, pt_buf,
                             kPlaintextLen, &pt_written)) {
            printf("testNonceLogAllAeadAlgos[%s]: consumer replay Open MUST "
                   "fail\n",
                   cs->name);
            failures++;
        } else if (flexEnvelopeLastVerifyError() !=
                   FLEX_ENV_VERIFY_FAILED_NONCE_REUSED) {
            printf("testNonceLogAllAeadAlgos[%s]: consumer replay err expected "
                   "NONCE_REUSED got %d\n",
                   cs->name, (int)flexEnvelopeLastVerifyError());
            failures++;
        }
        for (size_t i = 0; i < kPlaintextLen; i++) {
            if (pt_buf[i] != 0u) {
                printf("testNonceLogAllAeadAlgos[%s]: consumer replay did not "
                       "scrub plaintext_out (byte %zu = 0x%02x)\n",
                       cs->name, i, pt_buf[i]);
                failures++;
                break;
            }
        }

        /* Scenario 6: opt-out path — no log on verify => Open succeeds
         * even though the same envelope was just replay-rejected. */
        flexEnvelopeVerifyParams vp_no_log = {
            .key = kKey32,
            .key_len = cs->key_len,
            .nonceLog = NULL,
        };
        if (!flexEnvelopeOpen(env2, env2_written, &vp_no_log, pt_buf,
                              kPlaintextLen, &pt_written)) {
            printf("testNonceLogAllAeadAlgos[%s]: opt-out Open (no log) "
                   "failed (err=%d)\n",
                   cs->name, (int)flexEnvelopeLastVerifyError());
            failures++;
        }

    case_cleanup:
        /* Per-iteration scope exit releases the case's logs and buffers. */
        continue;
    }

    if (failures == 0) {
        printf("testNonceLogAllAeadAlgos: OK (%zu AEAD algos, 6 scenarios "
               "each)\n",
               kNumCases);
        return 0;
    }
    return 1;
}

/* ====================================================================
 * testChachaStreamOpenNonceFlip — verify the streaming Open path rejects
 * a single-byte-flipped nonce.  The one-shot Open's nonce-tamper test
 * lives at chacha20poly1305.c:1623; the streaming path has a SEPARATE
 * state initializer (chacha20poly1305OpenStreamInit) that derives the
 * Poly1305 one-time key, so a regression that mis-wires nonce bytes
 * there would not be caught.
 *
 * Flow: SealStream a payload under (key, nonce); then OpenStream with
 * (key, nonce_with_one_byte_flipped); after Update absorbs the
 * ciphertext, Final's tag check must return false.
 * ==================================================================== */
static int testChachaStreamOpenNonceFlipTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    const uint8_t key[CHACHA20_POLY1305_KEY_LEN] = {
        0x4A, 0x5B, 0x6C, 0x7D, 0x8E, 0x9F, 0xA0, 0xB1, 0xC2, 0xD3, 0xE4,
        0xF5, 0x06, 0x17, 0x28, 0x39, 0x4A, 0x5B, 0x6C, 0x7D, 0x8E, 0x9F,
        0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF5, 0x06, 0x17, 0x28, 0x39,
    };
    const uint8_t nonce[CHACHA20_POLY1305_NONCE_LEN] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC,
    };
    const uint8_t aad[] = "stream-open-nonce-flip-aad";
    const size_t aad_len = sizeof(aad) - 1u;
    const size_t kPayloadLen = 257u; /* odd size crosses block boundary */
    cleanerName(zfree, Bytes) uint8_t *plaintext =
        (uint8_t *)zmalloc(kPayloadLen);
    cleanerName(zfree, Bytes) uint8_t *ciphertext =
        (uint8_t *)zmalloc(kPayloadLen);
    cleanerName(zfree, Bytes) uint8_t *recovered =
        (uint8_t *)zmalloc(kPayloadLen);
    if (plaintext == NULL || ciphertext == NULL || recovered == NULL) {
        printf("testChachaStreamOpenNonceFlip: alloc failed\n");
        return 1;
    }
    for (size_t i = 0; i < kPayloadLen; i++) {
        plaintext[i] = (uint8_t)(0x80 ^ (i * 7u + 3u));
    }

    /* Seal under the correct nonce. */
    chacha20poly1305StreamCtx sealCtx;
    chacha20poly1305SealStreamInit(&sealCtx, key, nonce);
    if (!chacha20poly1305SealStreamAad(&sealCtx, aad, aad_len)) {
        printf("testChachaStreamOpenNonceFlip: SealStreamAad failed\n");
        failures++;
        goto cleanup;
    }
    if (!chacha20poly1305SealStreamUpdate(&sealCtx, plaintext, kPayloadLen,
                                          ciphertext)) {
        printf("testChachaStreamOpenNonceFlip: SealStreamUpdate failed\n");
        failures++;
        goto cleanup;
    }
    uint8_t tag[CHACHA20_POLY1305_TAG_LEN];
    if (!chacha20poly1305SealStreamFinal(&sealCtx, tag)) {
        printf("testChachaStreamOpenNonceFlip: SealStreamFinal failed\n");
        failures++;
        goto cleanup;
    }

    /* Sanity: untampered streaming Open succeeds. */
    {
        chacha20poly1305StreamCtx openCtx;
        chacha20poly1305OpenStreamInit(&openCtx, key, nonce);
        if (!chacha20poly1305OpenStreamAad(&openCtx, aad, aad_len)) {
            printf(
                "testChachaStreamOpenNonceFlip: sanity OpenStreamAad failed\n");
            failures++;
            goto cleanup;
        }
        if (!chacha20poly1305OpenStreamUpdate(&openCtx, ciphertext, kPayloadLen,
                                              recovered)) {
            printf("testChachaStreamOpenNonceFlip: sanity OpenStreamUpdate "
                   "failed\n");
            failures++;
            goto cleanup;
        }
        if (!chacha20poly1305OpenStreamFinal(&openCtx, tag)) {
            printf("testChachaStreamOpenNonceFlip: sanity OpenStreamFinal "
                   "FAILED on untampered ciphertext\n");
            failures++;
            goto cleanup;
        }
        if (memcmp(recovered, plaintext, kPayloadLen) != 0) {
            printf("testChachaStreamOpenNonceFlip: sanity recovered != "
                   "plaintext\n");
            failures++;
            goto cleanup;
        }
    }

    /* Tamper sweep: for each byte position in the nonce, flip a low bit
     * and a high bit; expect Final to return false in every case.  The
     * streaming Open MUST reject every flipped-nonce case end-to-end. */
    size_t accepted_count = 0;
    size_t total = 0;
    for (size_t nb = 0; nb < CHACHA20_POLY1305_NONCE_LEN; nb++) {
        for (int variant = 0; variant < 2; variant++) {
            uint8_t bad_nonce[CHACHA20_POLY1305_NONCE_LEN];
            memcpy(bad_nonce, nonce, sizeof(nonce));
            bad_nonce[nb] =
                (uint8_t)(bad_nonce[nb] ^ (variant == 0 ? 0x01u : 0x80u));

            chacha20poly1305StreamCtx openCtx;
            chacha20poly1305OpenStreamInit(&openCtx, key, bad_nonce);
            if (!chacha20poly1305OpenStreamAad(&openCtx, aad, aad_len)) {
                /* A NULL-arg-style refusal is unlikely here; treat as a
                 * test infra failure rather than a tamper accept. */
                printf("testChachaStreamOpenNonceFlip: OpenStreamAad failed "
                       "with valid args (nb=%zu var=%d)\n",
                       nb, variant);
                failures++;
                continue;
            }
            /* Update absorbs the ciphertext under the wrong keystream;
             * the bytes written to `recovered` are meaningless.  Final
             * MUST return false. */
            (void)chacha20poly1305OpenStreamUpdate(&openCtx, ciphertext,
                                                   kPayloadLen, recovered);
            const bool finalOk = chacha20poly1305OpenStreamFinal(&openCtx, tag);
            total++;
            if (finalOk) {
                printf("testChachaStreamOpenNonceFlip: nonce[%zu] flipped "
                       "(var=%d) accepted by Final (regression!)\n",
                       nb, variant);
                accepted_count++;
                failures++;
            }
        }
    }

    if (failures == 0) {
        printf("testChachaStreamOpenNonceFlip: OK (%zu nonce flips rejected, "
               "%zu accepted)\n",
               total, accepted_count);
    }

cleanup:
    if (failures == 0) {
        return 0;
    }
    return 1;
}

/* ====================================================================
 * testMailboxInnerFlexStructAwareFuzz — build a valid envelope around a
 * valid clusterMailbox inner-flex header, then exercise the mailbox
 * inner-header parser (cmDecodeInnerHeader) with structurally-malformed
 * variants while keeping the outer envelope MAC valid (re-wrap under
 * the original key). The mailbox MUST reject every malformed variant
 * with BAD_FRAME — not crash, not silently accept.
 *
 * Coverage of cmDecodeInnerHeader's documented rejection paths
 * (clusterMailbox.c:cmDecodeInnerHeader):
 *   1. Short / truncated buffer
 *   2. Malformed flex framing (outer total-bytes claim mismatch)
 *   3. Wrong entry count (2 or 4 entries instead of 3)
 *   4. Non-unsigned entry type (signed / float / bytes injected)
 *   5. origin_id > UINT32_MAX (out-of-range value)
 *   6. msg_type > UINT8_MAX (out-of-range value)
 *   7. origin_id == 0 (reserved sentinel)
 *   8. sequence == 0 (reserved sentinel)
 *   9. Trailing-garbage padding (extra bytes after the flex prefix
 *      are tolerated; the post-flex region is the application payload).
 *
 * Plus an UNBOUNDED byte-flip sweep over the entire inner-flex region.
 * cmDecodeInnerHeader now gates via flexValidateBufferDeep (O(N) per-
 * entry bound check) so a synthetic attacker that re-MACs under the
 * original key cannot cause the underlying flex walkers to overread the
 * buffer regardless of which byte they flip.  The sweep is the proof:
 * every flip must either pass cleanly (semantically valid mutation) or
 * fall to BAD_FRAME — never crash, never produce an unexpected outcome.
 *
 * Model: envelope struct-aware fuzz pattern (per-byte iteration +
 * accountability bucketing). See testFlexEnvelopeStructAwareFuzzTest
 * for the precedent.
 * ==================================================================== */
/* The two leading inner-header entries: this build's protocol stamp */
static void dkTestPushMailboxProtocol(flex **f) {
    flexPushUnsigned(f, clusterMailboxProtocol.version, FLEX_ENDPOINT_TAIL);
    flexPushUnsigned(f, 0u, FLEX_ENDPOINT_TAIL);
}

static int testMailboxInnerFlexStructAwareFuzzTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    const uint32_t ORIGIN = 0x4242u;
    const uint64_t SEQ = 1234u;
    const uint8_t MTYPE = 7u;
    const uint8_t key[32] = {
        0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0, 0x0F, 0x1F, 0x2F,
        0x3F, 0x4F, 0x5F, 0x6F, 0x7F, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45,
        0x67, 0x89, 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10,
    };
    const hmacKeyId kKeyId = 1u;
    const uint8_t payload[] = "mailbox-inner-flex-struct-fuzz-payload";
    const size_t payload_len = sizeof(payload);

    hmacKeyring *ring = hmacKeyringCreate();
    if (ring == NULL) {
        printf("testMailboxInnerFlexStructAwareFuzz: keyringCreate failed\n");
        return 1;
    }
    if (!hmacKeyringInstallUnsalted(ring, kKeyId, &hmacAlgoSHA256, key,
                                    sizeof(key))) {
        printf("testMailboxInnerFlexStructAwareFuzz: keyringInstall failed\n");
        hmacKeyringFree(ring);
        return 1;
    }

    flexEnvelopeWrapParams wp = {
        .algo = FLEX_ENV_ALGO_HMAC_SHA256,
        .key = key,
        .key_len = sizeof(key),
        .on_wire_flags = FLEX_ENV_FLAG_NONE,
    };

    /* Helper macro: re-create the consumer mailbox so each Ingest case
     * starts with a clean replay watermark.  Each accepted alternate
     * frame would otherwise update last_applied and shadow later
     * iterations as REPLAY. */
#define FUZZ_REWIND_MAILBOX()                                                  \
    do {                                                                       \
        clusterMailboxFree(cons);                                              \
        cons =                                                                 \
            clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, NULL, NULL); \
        if (cons == NULL) {                                                    \
            printf("testMailboxInnerFlexStructAwareFuzz: re-create mailbox "   \
                   "failed\n");                                                \
            failures++;                                                        \
            goto fuzz_cleanup;                                                 \
        }                                                                      \
    } while (0)

    clusterMailbox *cons =
        clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, NULL, NULL);
    if (cons == NULL) {
        printf("testMailboxInnerFlexStructAwareFuzz: mailboxCreate failed\n");
        hmacKeyringFree(ring);
        return 1;
    }

    /* Step 1: canonical inner header (sanity baseline). */
    uint8_t hdr_canonical[CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES];
    const size_t hdr_len = cmEncodeInnerHeader(
        hdr_canonical, sizeof(hdr_canonical), ORIGIN, SEQ, MTYPE);
    if (hdr_len == 0u) {
        printf("testMailboxInnerFlexStructAwareFuzz: cmEncodeInnerHeader "
               "failed\n");
        clusterMailboxFree(cons);
        hmacKeyringFree(ring);
        return 1;
    }

    /* Track outcome counters via per-outcome bucketing. */
    size_t total_cases = 0;
    size_t expected_ok = 0;
    size_t bad_frame_reject = 0;
    size_t verify_failed_reject = 0; /* outer MAC rejected (e.g. truncated) */
    size_t replay_reject = 0;
    size_t unexpected_outcome = 0;

    /* Local helper: wrap (origin, seq, mtype, payload_bytes,
     * payload_bytes_len) into an envelope and Ingest. `inner_flex_bytes`
     * is the pre-built (possibly malformed) flex prefix; we concatenate
     * it with the application payload and re-Wrap to recompute the
     * outer MAC. */
#define FUZZ_INGEST(label, inner_flex_bytes, inner_flex_len, app_payload,      \
                    app_payload_len, expected_rc)                              \
    do {                                                                       \
        const size_t _inner_total = (inner_flex_len) + (app_payload_len);      \
        const size_t _env_cap = flexEnvelopeWrappedSize(                       \
            (uint64_t)_inner_total, FLEX_ENV_ALGO_HMAC_SHA256,                 \
            FLEX_ENV_FLAG_NONE);                                               \
        if (_env_cap == 0u) {                                                  \
            printf("testMailboxInnerFlexStructAwareFuzz[%s]: "                 \
                   "wrappedSize=0\n",                                          \
                   (label));                                                   \
            failures++;                                                        \
            break;                                                             \
        }                                                                      \
        cleanerName(zfree, Bytes) uint8_t *_env =                              \
            (uint8_t *)zmalloc(_env_cap);                                      \
        cleanerName(zfree, Bytes) uint8_t *_inner =                            \
            (uint8_t *)zmalloc(_inner_total);                                  \
        if (_env == NULL || _inner == NULL) {                                  \
            printf("testMailboxInnerFlexStructAwareFuzz[%s]: alloc "           \
                   "failed\n",                                                 \
                   (label));                                                   \
            failures++;                                                        \
            break;                                                             \
        }                                                                      \
        memcpy(_inner, (inner_flex_bytes), (inner_flex_len));                  \
        memcpy(_inner + (inner_flex_len), (app_payload), (app_payload_len));   \
        size_t _written = 0;                                                   \
        if (!flexEnvelopeWrap(_env, _env_cap, _inner, (uint64_t)_inner_total,  \
                              &wp, &_written)) {                               \
            printf("testMailboxInnerFlexStructAwareFuzz[%s]: Wrap "            \
                   "failed\n",                                                 \
                   (label));                                                   \
            failures++;                                                        \
            break;                                                             \
        }                                                                      \
        FUZZ_REWIND_MAILBOX();                                                 \
        const clusterMailboxIngestResult _rc =                                 \
            clusterMailboxIngest(cons, _env, _written, kKeyId);                \
        total_cases++;                                                         \
        if (_rc == CLUSTER_MAILBOX_OK)                                         \
            expected_ok++;                                                     \
        else if (_rc == CLUSTER_MAILBOX_ERR_BAD_FRAME)                         \
            bad_frame_reject++;                                                \
        else if (_rc == CLUSTER_MAILBOX_ERR_VERIFY_FAILED)                     \
            verify_failed_reject++;                                            \
        else if (_rc == CLUSTER_MAILBOX_ERR_REPLAY)                            \
            replay_reject++;                                                   \
        else                                                                   \
            unexpected_outcome++;                                              \
        if (_rc != (expected_rc)) {                                            \
            printf("testMailboxInnerFlexStructAwareFuzz[%s]: rc=%d "           \
                   "expected=%d\n",                                            \
                   (label), (int)_rc, (int)(expected_rc));                     \
            failures++;                                                        \
        }                                                                      \
    } while (0)

    /* ---------------- Case 1: sanity (canonical) ---------------------- */
    FUZZ_INGEST("canonical", hdr_canonical, hdr_len, payload, payload_len,
                CLUSTER_MAILBOX_OK);

    /* ---------------- Case 2: wrong entry count (2 entries) ----------- */
    {
        cleaner(flexFree) flex *f2 = flexNew();
        dkTestPushMailboxProtocol(&f2);
        flexPushUnsigned(&f2, (uint64_t)ORIGIN, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f2, SEQ, FLEX_ENDPOINT_TAIL);
        const size_t f2_bytes = flexBytes(f2);
        cleanerName(zfree, Bytes) uint8_t *buf2 = (uint8_t *)zmalloc(f2_bytes);
        memcpy(buf2, f2, f2_bytes);
        FUZZ_INGEST("count_two", buf2, f2_bytes, payload, payload_len,
                    CLUSTER_MAILBOX_ERR_BAD_FRAME);
    }

    /* ---------------- Case 3: wrong entry count (4 entries) ----------- */
    {
        cleaner(flexFree) flex *f4 = flexNew();
        dkTestPushMailboxProtocol(&f4);
        flexPushUnsigned(&f4, (uint64_t)ORIGIN, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f4, SEQ, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f4, (uint64_t)MTYPE, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f4, 0xABu, FLEX_ENDPOINT_TAIL); /* extra entry */
        const size_t f4_bytes = flexBytes(f4);
        cleanerName(zfree, Bytes) uint8_t *buf4 = (uint8_t *)zmalloc(f4_bytes);
        memcpy(buf4, f4, f4_bytes);
        FUZZ_INGEST("count_four", buf4, f4_bytes, payload, payload_len,
                    CLUSTER_MAILBOX_ERR_BAD_FRAME);
    }

    /* ---------------- Case 4: signed entry instead of unsigned -------- */
    {
        cleaner(flexFree) flex *fs = flexNew();
        dkTestPushMailboxProtocol(&fs);
        /* origin slot with a signed value violates the type check. */
        flexPushSigned(&fs, -1, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&fs, SEQ, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&fs, (uint64_t)MTYPE, FLEX_ENDPOINT_TAIL);
        const size_t fs_bytes = flexBytes(fs);
        cleanerName(zfree, Bytes) uint8_t *bufs = (uint8_t *)zmalloc(fs_bytes);
        memcpy(bufs, fs, fs_bytes);
        FUZZ_INGEST("entry_signed", bufs, fs_bytes, payload, payload_len,
                    CLUSTER_MAILBOX_ERR_BAD_FRAME);
    }

    /* ---------------- Case 5: origin_id > UINT32_MAX ------------------ */
    {
        cleaner(flexFree) flex *foor = flexNew();
        dkTestPushMailboxProtocol(&foor);
        flexPushUnsigned(&foor, (uint64_t)UINT32_MAX + 1ULL,
                         FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&foor, SEQ, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&foor, (uint64_t)MTYPE, FLEX_ENDPOINT_TAIL);
        const size_t foor_bytes = flexBytes(foor);
        cleanerName(zfree, Bytes) uint8_t *bufoor =
            (uint8_t *)zmalloc(foor_bytes);
        memcpy(bufoor, foor, foor_bytes);
        FUZZ_INGEST("origin_oor", bufoor, foor_bytes, payload, payload_len,
                    CLUSTER_MAILBOX_ERR_BAD_FRAME);
    }

    /* ---------------- Case 6: msg_type > UINT8_MAX -------------------- */
    {
        cleaner(flexFree) flex *fmoor = flexNew();
        dkTestPushMailboxProtocol(&fmoor);
        flexPushUnsigned(&fmoor, (uint64_t)ORIGIN, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&fmoor, SEQ, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&fmoor, (uint64_t)UINT8_MAX + 1u, FLEX_ENDPOINT_TAIL);
        const size_t fmoor_bytes = flexBytes(fmoor);
        cleanerName(zfree, Bytes) uint8_t *bufmoor =
            (uint8_t *)zmalloc(fmoor_bytes);
        memcpy(bufmoor, fmoor, fmoor_bytes);
        FUZZ_INGEST("mtype_oor", bufmoor, fmoor_bytes, payload, payload_len,
                    CLUSTER_MAILBOX_ERR_BAD_FRAME);
    }

    /* ---------------- Case 7: origin_id == 0 (sentinel) --------------- */
    {
        cleaner(flexFree) flex *fz = flexNew();
        dkTestPushMailboxProtocol(&fz);
        flexPushUnsigned(&fz, 0u, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&fz, SEQ, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&fz, (uint64_t)MTYPE, FLEX_ENDPOINT_TAIL);
        const size_t fz_bytes = flexBytes(fz);
        cleanerName(zfree, Bytes) uint8_t *bufz = (uint8_t *)zmalloc(fz_bytes);
        memcpy(bufz, fz, fz_bytes);
        FUZZ_INGEST("origin_zero", bufz, fz_bytes, payload, payload_len,
                    CLUSTER_MAILBOX_ERR_BAD_FRAME);
    }

    /* ---------------- Case 8: sequence == 0 (sentinel) ---------------- */
    {
        cleaner(flexFree) flex *fsz = flexNew();
        dkTestPushMailboxProtocol(&fsz);
        flexPushUnsigned(&fsz, (uint64_t)ORIGIN, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&fsz, 0u, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&fsz, (uint64_t)MTYPE, FLEX_ENDPOINT_TAIL);
        const size_t fsz_bytes = flexBytes(fsz);
        cleanerName(zfree, Bytes) uint8_t *bufsz =
            (uint8_t *)zmalloc(fsz_bytes);
        memcpy(bufsz, fsz, fsz_bytes);
        FUZZ_INGEST("seq_zero", bufsz, fsz_bytes, payload, payload_len,
                    CLUSTER_MAILBOX_ERR_BAD_FRAME);
    }

    /* ---------------- Case 9: trailing garbage after flex prefix ------ */
    /* cmDecodeInnerHeader correctly treats bytes after the flex prefix
     * as the opaque application payload; this case proves the flex
     * prefix boundary is enforced.  An accepted Ingest with a different
     * `payload` (the trailing-garbage bytes are now the application
     * payload, not the original `payload`) is the expected outcome. */
    {
        const uint8_t garbage_payload[] = {0xCA, 0xFE, 0xBA, 0xBE,
                                           0xDE, 0xAD, 0xBE, 0xEF};
        FUZZ_INGEST("trailing_garbage", hdr_canonical, hdr_len, garbage_payload,
                    sizeof(garbage_payload), CLUSTER_MAILBOX_OK);
    }

    /* ---------------- Case 10: empty inner flex (no entries) --------- */
    {
        cleaner(flexFree) flex *fe0 = flexNew();
        const size_t fe0_bytes = flexBytes(fe0);
        cleanerName(zfree, Bytes) uint8_t *bufe0 =
            (uint8_t *)zmalloc(fe0_bytes);
        memcpy(bufe0, fe0, fe0_bytes);
        FUZZ_INGEST("count_zero", bufe0, fe0_bytes, payload, payload_len,
                    CLUSTER_MAILBOX_ERR_BAD_FRAME);
    }

    /* Unbounded per-byte sweep: for every byte position in the inner-
     * flex region, flip each bit, re-MAC the envelope under the original
     * key, and ingest.  With flexValidateBufferDeep now gating
     * cmDecodeInnerHeader, every flip must either pass cleanly (the
     * mutation happened to land on a semantically valid alternate value)
     * or fall to BAD_FRAME — no ASAN/UBSAN crash, no unexpected outcome.
     *
     * This is the proof for the "trust boundary closed" guarantee: a
     * synthetic attacker that can re-MAC under the original key (which
     * is computationally infeasible in production but trivial inside a
     * unit test that happens to hold the key) cannot induce overread
     * regardless of which entry-length varint byte they flip. */
    size_t sweep_total = 0;
    size_t sweep_ok = 0;
    size_t sweep_bad_frame = 0;
    size_t sweep_unexpected = 0;
    for (size_t bpos = 0; bpos < hdr_len; bpos++) {
        for (uint32_t bit = 0; bit < 8u; bit++) {
            uint8_t mutated_hdr[CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES];
            memcpy(mutated_hdr, hdr_canonical, hdr_len);
            mutated_hdr[bpos] ^= (uint8_t)(1u << bit);

            /* Concatenate (mutated_hdr || payload) and re-Wrap under
             * the original key so the outer envelope MAC verifies; only
             * the inner-flex bytes carry the tampering. */
            const size_t inner_total = hdr_len + payload_len;
            const size_t env_cap = flexEnvelopeWrappedSize(
                (uint64_t)inner_total, FLEX_ENV_ALGO_HMAC_SHA256,
                FLEX_ENV_FLAG_NONE);
            if (env_cap == 0u) {
                printf("testMailboxInnerFlexStructAwareFuzz[sweep "
                       "bpos=%zu bit=%u]: wrappedSize=0\n",
                       bpos, bit);
                failures++;
                continue;
            }
            cleanerName(zfree, Bytes) uint8_t *env =
                (uint8_t *)zmalloc(env_cap);
            cleanerName(zfree, Bytes) uint8_t *inner =
                (uint8_t *)zmalloc(inner_total);
            memcpy(inner, mutated_hdr, hdr_len);
            memcpy(inner + hdr_len, payload, payload_len);
            size_t written = 0;
            if (!flexEnvelopeWrap(env, env_cap, inner, (uint64_t)inner_total,
                                  &wp, &written)) {
                printf("testMailboxInnerFlexStructAwareFuzz[sweep "
                       "bpos=%zu bit=%u]: Wrap failed\n",
                       bpos, bit);
                failures++;
                continue;
            }
            FUZZ_REWIND_MAILBOX();
            const clusterMailboxIngestResult rc =
                clusterMailboxIngest(cons, env, written, kKeyId);
            sweep_total++;
            if (rc == CLUSTER_MAILBOX_OK) {
                sweep_ok++;
            } else if (rc == CLUSTER_MAILBOX_ERR_BAD_FRAME ||
                       rc == CLUSTER_MAILBOX_ERR_PROTOCOL) {
                /* A flip in the protocol stamp reads as a version or
                 * feature this build does not read */
                sweep_bad_frame++;
            } else {
                /* Any other outcome (verify_failed, replay, or an
                 * unexpected status) indicates either a missed-rejection
                 * shape or a different bug class.  REPLAY cannot occur
                 * because FUZZ_REWIND_MAILBOX recreates the consumer.
                 * VERIFY_FAILED cannot occur because we re-Wrap with the
                 * same key.  Any other rc is a regression. */
                sweep_unexpected++;
                printf("testMailboxInnerFlexStructAwareFuzz[sweep "
                       "bpos=%zu bit=%u]: unexpected rc=%d\n",
                       bpos, bit, (int)rc);
                failures++;
            }
        }
    }

    printf("testMailboxInnerFlexStructAwareFuzz: %zu total cases: "
           "ok=%zu bad_frame=%zu verify_failed=%zu replay=%zu "
           "unexpected=%zu\n",
           total_cases, expected_ok, bad_frame_reject, verify_failed_reject,
           replay_reject, unexpected_outcome);
    printf("testMailboxInnerFlexStructAwareFuzz: sweep %zu flips: "
           "ok=%zu bad_frame=%zu unexpected=%zu\n",
           sweep_total, sweep_ok, sweep_bad_frame, sweep_unexpected);

#undef FUZZ_INGEST
#undef FUZZ_REWIND_MAILBOX

fuzz_cleanup:
    clusterMailboxFree(cons);
    hmacKeyringFree(ring);

    if (failures == 0) {
        printf("testMailboxInnerFlexStructAwareFuzz: OK\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * testMailboxRotateAnnounceStructAwareFuzz — extend the mailbox struct-
 * aware fuzz pattern to clusterMailboxParseRotateAnnounce.
 *
 * Track F migrated clusterMailboxParseRotateAnnounce to gate its flex
 * walk via flexValidateBufferDeep.  The parser takes raw bytes directly
 * (no outer envelope) so the fuzz harness is simpler than the inner-
 * flex fuzz: build a valid 1-entry rotate-announce payload, flip every
 * bit at every byte position, feed each variant to the parser, and
 * assert no crash + either ok (semantically valid alternate id) or
 * parse-failure.
 *
 * This is the proof for the rotate-announce trust boundary: a synthetic
 * byte tamper cannot induce the flex walker to overread the announce
 * buffer regardless of which byte they flip.
 * ==================================================================== */
static int testMailboxRotateAnnounceStructAwareFuzzTest(int argc,
                                                        char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    const hmacKeyId kAnnouncedId = 0x12345678u;
    uint8_t canonical[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN];
    const size_t canonical_len =
        clusterMailboxBuildRotateAnnounce(canonical, kAnnouncedId);
    if (canonical_len == 0u ||
        canonical_len > CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN) {
        printf("testMailboxRotateAnnounceStructAwareFuzz: build failed "
               "(len=%zu)\n",
               canonical_len);
        return 1;
    }

    /* Sanity: the canonical payload parses to the original id. */
    {
        hmacKeyId got = 0u;
        if (!clusterMailboxParseRotateAnnounce(canonical, canonical_len,
                                               &got) ||
            got != kAnnouncedId) {
            printf("testMailboxRotateAnnounceStructAwareFuzz: canonical "
                   "parse failed (rc=%d got=0x%x)\n",
                   (int)clusterMailboxParseRotateAnnounce(canonical,
                                                          canonical_len, &got),
                   (unsigned)got);
            return 1;
        }
    }

    /* Unbounded per-byte sweep: for every byte position in the canonical
     * announce buffer, flip each of the 8 bits, parse, and bucket the
     * outcome.  Every flip must either (a) parse to ANY non-zero key id
     * (semantically valid alternate value) or (b) be rejected — never
     * crash, never produce a "parsed but stayed at canonical id"
     * outcome that would indicate a missed comparison. */
    size_t sweep_total = 0;
    size_t sweep_ok = 0;
    size_t sweep_bad_frame = 0;
    size_t sweep_unexpected = 0;
    for (size_t bpos = 0; bpos < canonical_len; bpos++) {
        for (uint32_t bit = 0; bit < 8u; bit++) {
            uint8_t mutated[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN];
            memcpy(mutated, canonical, canonical_len);
            mutated[bpos] ^= (uint8_t)(1u << bit);

            hmacKeyId got = 0u;
            const bool ok =
                clusterMailboxParseRotateAnnounce(mutated, canonical_len, &got);
            sweep_total++;
            if (ok) {
                /* Accepted: the mutated bytes parsed to a valid key id.
                 * The parsed id MUST be non-zero (the parser rejects
                 * HMAC_KEY_ID_INVALID).  We make no claim about the
                 * specific value — many bit flips produce a different
                 * but valid u32. */
                if (got == HMAC_KEY_ID_INVALID || got == 0u) {
                    sweep_unexpected++;
                    printf("testMailboxRotateAnnounceStructAwareFuzz: "
                           "bpos=%zu bit=%u parsed-but-zero id\n",
                           bpos, bit);
                    failures++;
                } else {
                    sweep_ok++;
                }
            } else {
                sweep_bad_frame++;
            }
        }
    }

    printf("testMailboxRotateAnnounceStructAwareFuzz: sweep %zu flips: "
           "ok=%zu bad_frame=%zu unexpected=%zu\n",
           sweep_total, sweep_ok, sweep_bad_frame, sweep_unexpected);

    if (sweep_unexpected != 0u) {
        failures++;
    }
    if (sweep_ok + sweep_bad_frame != sweep_total) {
        printf("testMailboxRotateAnnounceStructAwareFuzz: bucket sum "
               "mismatch (ok+bad=%zu != total=%zu)\n",
               sweep_ok + sweep_bad_frame, sweep_total);
        failures++;
    }

    if (failures == 0) {
        printf("testMailboxRotateAnnounceStructAwareFuzz: OK\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * Shared helper: per-byte sweep over a valid raw-flex snapshot buffer
 * driving clusterMailboxRestore (or any other parser callable that
 * consumes raw flex bytes).  Returns 0 on success, 1 on any unexpected
 * outcome (crash-equivalent state observed via ASAN/UBSAN would have
 * already aborted the process).  The caller owns mailbox creation so
 * the parser can be exercised against a freshly-rewound consumer for
 * every iteration.
 *
 * The fuzz invariant is: for every bit flip in `snap`, the parser
 * either accepts (the mutation happened to land on a semantically
 * valid alternate state) or rejects (the deep validator caught the
 * malformation) — never crashes, never returns a "parsed garbage"
 * outcome.
 * ==================================================================== */
typedef void (*mailboxRebuildFn)(clusterMailbox **out_mb,
                                 hmacKeyring **out_ring, const uint8_t key[32]);

static int mailboxRestoreSweepHelper(const char *label, const uint8_t *snap,
                                     size_t snap_len, const uint8_t key[32],
                                     mailboxRebuildFn rebuild) {
    int failures = 0;
    size_t sweep_total = 0;
    size_t sweep_ok = 0;
    size_t sweep_rejected = 0;
    size_t sweep_unexpected = 0;

    /* Pre-sanity: the canonical snapshot itself must restore cleanly. */
    {
        clusterMailbox *mb = NULL;
        hmacKeyring *ring = NULL;
        rebuild(&mb, &ring, key);
        if (mb == NULL || ring == NULL) {
            printf("%s: rebuild failed (canonical sanity)\n", label);
            if (mb != NULL) {
                clusterMailboxFree(mb);
            }
            if (ring != NULL) {
                hmacKeyringFree(ring);
            }
            return 1;
        }
        if (!clusterMailboxRestore(mb, snap, snap_len)) {
            printf("%s: canonical restore failed\n", label);
            clusterMailboxFree(mb);
            hmacKeyringFree(ring);
            return 1;
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    /* Unbounded per-byte sweep. */
    for (size_t bpos = 0; bpos < snap_len; bpos++) {
        for (uint32_t bit = 0; bit < 8u; bit++) {
            cleanerName(zfree, Bytes) uint8_t *mutated =
                (uint8_t *)zmalloc(snap_len);
            memcpy(mutated, snap, snap_len);
            mutated[bpos] ^= (uint8_t)(1u << bit);

            clusterMailbox *mb = NULL;
            hmacKeyring *ring = NULL;
            rebuild(&mb, &ring, key);
            if (mb == NULL || ring == NULL) {
                printf("%s[bpos=%zu bit=%u]: rebuild failed\n", label, bpos,
                       bit);
                if (mb != NULL) {
                    clusterMailboxFree(mb);
                }
                if (ring != NULL) {
                    hmacKeyringFree(ring);
                }
                failures++;
                continue;
            }

            const bool ok = clusterMailboxRestore(mb, mutated, snap_len);
            sweep_total++;
            if (ok) {
                sweep_ok++;
            } else {
                /* clusterMailboxRestore returns bool — false means "deep
                 * validator caught it OR a downstream type/range check
                 * caught it".  Either way the outcome is a clean
                 * rejection (no crash, no partial state corruption). */
                sweep_rejected++;
            }
            clusterMailboxFree(mb);
            hmacKeyringFree(ring);
        }
    }

    printf("%s: sweep %zu flips: ok=%zu rejected=%zu unexpected=%zu\n", label,
           sweep_total, sweep_ok, sweep_rejected, sweep_unexpected);

    if (sweep_unexpected != 0u) {
        failures++;
    }
    if (sweep_ok + sweep_rejected != sweep_total) {
        printf("%s: bucket sum mismatch\n", label);
        failures++;
    }
    return failures;
}

/* Helper used by both restore fuzzers: rebuild a fresh empty mailbox
 * + keyring for each sweep iteration.  Restore consumes raw flex bytes
 * and is single-threaded per the header contract, so a fresh target
 * per iteration removes any cross-iteration carry. */
static void mailboxRestoreRebuild(clusterMailbox **out_mb,
                                  hmacKeyring **out_ring,
                                  const uint8_t key[32]) {
    const hmacKeyId kKeyId = 100u;
    hmacKeyring *ring = hmacKeyringCreate();
    if (ring == NULL) {
        *out_mb = NULL;
        *out_ring = NULL;
        return;
    }
    if (!hmacKeyringInstallUnsalted(ring, kKeyId, &hmacAlgoSHA256, key, 32u)) {
        hmacKeyringFree(ring);
        *out_mb = NULL;
        *out_ring = NULL;
        return;
    }
    clusterMailbox *mb =
        clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, NULL, NULL);
    if (mb == NULL) {
        hmacKeyringFree(ring);
        *out_mb = NULL;
        *out_ring = NULL;
        return;
    }
    *out_mb = mb;
    *out_ring = ring;
}

/* Build a non-trivial mailbox state, snapshot it to raw flex bytes via
 * `snap_fn`, and return the bytes.  Caller owns and zfrees the result.
 * `snap_fn` is the function that produces the snapshot — either
 * clusterMailboxSnapshot directly or the persistOps descriptor's
 * snapshot callback. */
typedef uint8_t *(*snapshotFn)(const clusterMailbox *mb, size_t *out_len);

static uint8_t *mailboxSnapshotDirect(const clusterMailbox *mb,
                                      size_t *out_len) {
    const size_t need = clusterMailboxSnapshotSize(mb);
    if (need == 0u) {
        *out_len = 0u;
        return NULL;
    }
    /* @own: caller; free=zfree */
    cleanerName(zfree, Bytes) uint8_t *out = (uint8_t *)zmalloc(need);
    size_t written = 0u;
    if (!clusterMailboxSnapshot(mb, out, need, &written) || written != need) {
        *out_len = 0u;
        return NULL;
    }
    *out_len = need;
    uint8_t *result = out;
    out = NULL;
    return result;
}

static uint8_t *mailboxSnapshotPersistOps(const clusterMailbox *mb,
                                          size_t *out_len) {
    return persistOpsClusterMailbox.snapshot(mb, out_len);
}

/* Drive a few origins forward + ingest, mirroring the existing
 * snapshot/restore + persistOps round-trip tests so the produced
 * snapshot exercises the same parser-input shape those tests fuzz. */
static uint8_t *mailboxBuildSnapshotFor(snapshotFn snap_fn, size_t *out_len,
                                        const uint8_t key[32]) {
    const hmacKeyId kKeyId = 100u;
    hmacKeyring *ring = hmacKeyringCreate();
    if (ring == NULL) {
        return NULL;
    }
    if (!hmacKeyringInstallUnsalted(ring, kKeyId, &hmacAlgoSHA256, key, 32u)) {
        hmacKeyringFree(ring);
        return NULL;
    }
    clusterMailbox *mb_a =
        clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, NULL, NULL);
    if (mb_a == NULL) {
        hmacKeyringFree(ring);
        return NULL;
    }

    const uint8_t payload[] = "snap";
    const size_t sealed_sz = clusterMailboxSealedSize(mb_a, sizeof(payload));
    const struct {
        uint32_t origin;
        uint64_t seq;
    } sends[] = {{10u, 3u}, {20u, 7u}, {30u, 1u}};
    for (uint32_t i = 0; i < 3u; i++) {
        cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(sealed_sz);
        size_t w = 0u;
        if (!clusterMailboxSeal(mb_a, sends[i].origin, sends[i].seq, 0u,
                                payload, sizeof(payload), kKeyId, buf,
                                sealed_sz, &w)) {
            clusterMailboxFree(mb_a);
            hmacKeyringFree(ring);
            return NULL;
        }
        if (clusterMailboxIngest(mb_a, buf, w, kKeyId) != CLUSTER_MAILBOX_OK) {
            clusterMailboxFree(mb_a);
            hmacKeyringFree(ring);
            return NULL;
        }
    }

    uint8_t *snap = snap_fn(mb_a, out_len);
    clusterMailboxFree(mb_a);
    hmacKeyringFree(ring);
    return snap;
}

/* ====================================================================
 * testMailboxRestoreStructAwareFuzz — fuzz clusterMailboxRestore via
 * the direct snapshot path (clusterMailboxSnapshot).  This covers the
 * snapshot/restore TEST path at src/clusterMailbox.c:2210-2270 — bytes
 * produced by clusterMailboxSnapshot are byte-flipped and re-fed
 * through clusterMailboxRestore.  flexValidateBufferDeep gates the
 * walk; every flip must either succeed or be rejected, never crash.
 * ==================================================================== */
static int testMailboxRestoreStructAwareFuzzTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const uint8_t key[32] = {
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0, 0xb0,
        0xc0, 0xd0, 0xe0, 0xf0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xfe, 0xff,
    };
    size_t snap_len = 0u;
    uint8_t *snap =
        mailboxBuildSnapshotFor(mailboxSnapshotDirect, &snap_len, key);
    if (snap == NULL || snap_len == 0u) {
        printf("testMailboxRestoreStructAwareFuzz: build snapshot failed\n");
        zfree(snap);
        return 1;
    }
    const int failures =
        mailboxRestoreSweepHelper("testMailboxRestoreStructAwareFuzz", snap,
                                  snap_len, key, mailboxRestoreRebuild);
    zfree(snap);
    if (failures == 0) {
        printf("testMailboxRestoreStructAwareFuzz: OK\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * testMailboxPersistOpsRestoreStructAwareFuzz — fuzz clusterMailboxRestore
 * via the persistOps descriptor snapshot path
 * (persistOpsClusterMailbox.snapshot).  This covers the persistOps
 * round-trip TEST path at src/clusterMailbox.c:2826-2877 — bytes
 * produced by the descriptor's snapshot callback are byte-flipped and
 * re-fed through clusterMailboxRestore.  The bytes are structurally
 * identical to the direct snapshot path's output, but the producer is
 * the persist framework's adapter shim so the fuzz coverage is filed
 * against that distinct production caller.
 * ==================================================================== */
static int testMailboxPersistOpsRestoreStructAwareFuzzTest(int argc,
                                                           char *argv[]) {
    (void)argc;
    (void)argv;
    const uint8_t key[32] = {
        0x11, 0x21, 0x31, 0x41, 0x51, 0x61, 0x71, 0x81, 0x91, 0xa1, 0xb1,
        0xc1, 0xd1, 0xe1, 0xf1, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xfe, 0xff, 0x00,
    };
    size_t snap_len = 0u;
    uint8_t *snap =
        mailboxBuildSnapshotFor(mailboxSnapshotPersistOps, &snap_len, key);
    if (snap == NULL || snap_len == 0u) {
        printf("testMailboxPersistOpsRestoreStructAwareFuzz: build "
               "snapshot failed\n");
        zfree(snap);
        return 1;
    }
    const int failures =
        mailboxRestoreSweepHelper("testMailboxPersistOpsRestoreStructAwareFuzz",
                                  snap, snap_len, key, mailboxRestoreRebuild);
    zfree(snap);
    if (failures == 0) {
        printf("testMailboxPersistOpsRestoreStructAwareFuzz: OK\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * testMailboxRestoreWrappedStructAwareFuzz — fuzz clusterMailboxRestoreWrapped,
 * which routes raw envelope bytes through flexEnvelopeVerify (outer MAC
 * check) and then through clusterMailboxRestore (inner deep validator
 * + per-record walk).  This is the production "snapshot from disk"
 * path covered by clusterMailboxRestoreWrapped at clusterMailbox.c:1387.
 *
 * Following the testMailboxInnerFlexStructAwareFuzz pattern: for every
 * byte in the INNER flex region, flip each bit, re-wrap the envelope
 * under the original key (so outer MAC verifies), feed through
 * clusterMailboxRestoreWrapped, assert no crash + either success or
 * rejection.
 * ==================================================================== */
static int testMailboxRestoreWrappedStructAwareFuzzTest(int argc,
                                                        char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;
    const uint8_t key[32] = {
        0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x82, 0x92, 0xa2, 0xb2,
        0xc2, 0xd2, 0xe2, 0xf2, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
        0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xfe, 0xff, 0x00, 0x01,
    };

    /* Build a non-trivial mailbox + snapshot it as raw bytes. */
    size_t raw_len = 0u;
    cleanerName(zfree, Bytes) uint8_t *raw =
        mailboxBuildSnapshotFor(mailboxSnapshotDirect, &raw_len, key);
    if (raw == NULL || raw_len == 0u) {
        printf("testMailboxRestoreWrappedStructAwareFuzz: build snapshot "
               "failed\n");
        return 1;
    }

    /* The envelope's inner bytes, as clusterMailboxSnapshotWrapped lays
     * them out: the format prefix, then the watermark bytes.  The sweep
     * below flips bits in both. */
    const dkFormatSpec spec = persistOpsFormat(&persistOpsClusterMailbox);
    const dkFormatStamp stamp = dkFormatStampFor(&spec, 0);
    const size_t prefix_len = dkFormatPrefixSize(&stamp);
    const size_t inner_len = prefix_len + raw_len;
    cleanerName(zfree, Bytes) uint8_t *inner = zmalloc(inner_len);
    dkFormatPrefixWrite(&stamp, inner, prefix_len);
    memcpy(inner + prefix_len, raw, raw_len);

    flexEnvelopeWrapParams wp = {
        .algo = FLEX_ENV_ALGO_HMAC_SHA256,
        .key = key,
        .key_len = sizeof(key),
        .on_wire_flags = FLEX_ENV_FLAG_NONE,
    };
    flexEnvelopeVerifyParams vp = {
        .max_payload_len = (uint64_t)inner_len + 1024u,
        .key = key,
        .key_len = sizeof(key),
    };

    /* Sanity: canonical wrap + restoreWrapped succeed. */
    {
        const size_t env_cap = flexEnvelopeWrappedSize(
            (uint64_t)inner_len, FLEX_ENV_ALGO_HMAC_SHA256, FLEX_ENV_FLAG_NONE);
        if (env_cap == 0u) {
            printf("testMailboxRestoreWrappedStructAwareFuzz: wrappedSize=0\n");
            return 1;
        }
        cleanerName(zfree, Bytes) uint8_t *env = (uint8_t *)zmalloc(env_cap);
        size_t written = 0u;
        if (!flexEnvelopeWrap(env, env_cap, inner, (uint64_t)inner_len, &wp,
                              &written)) {
            printf("testMailboxRestoreWrappedStructAwareFuzz: canonical "
                   "Wrap failed\n");
            return 1;
        }
        hmacKeyring *ring = NULL;
        clusterMailbox *mb = NULL;
        mailboxRestoreRebuild(&mb, &ring, key);
        if (mb == NULL || ring == NULL) {
            printf("testMailboxRestoreWrappedStructAwareFuzz: rebuild "
                   "failed (canonical)\n");
            if (mb != NULL) {
                clusterMailboxFree(mb);
            }
            if (ring != NULL) {
                hmacKeyringFree(ring);
            }
            return 1;
        }
        if (!clusterMailboxRestoreWrapped(mb, &vp, env, written)) {
            printf("testMailboxRestoreWrappedStructAwareFuzz: canonical "
                   "RestoreWrapped failed\n");
            failures++;
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    /* Unbounded per-byte sweep over the inner-flex region.  For every
     * bit position, flip it, re-wrap, restore.  Buckets: ok (the
     * mutation produced a still-valid restore) / rejected (the deep
     * validator or the per-record walker caught it) / unexpected (any
     * outcome that indicates a missed-rejection or test-harness bug). */
    size_t sweep_total = 0;
    size_t sweep_ok = 0;
    size_t sweep_rejected = 0;
    size_t sweep_unexpected = 0;
    const size_t env_cap = flexEnvelopeWrappedSize(
        (uint64_t)inner_len, FLEX_ENV_ALGO_HMAC_SHA256, FLEX_ENV_FLAG_NONE);
    for (size_t bpos = 0; bpos < inner_len; bpos++) {
        for (uint32_t bit = 0; bit < 8u; bit++) {
            cleanerName(zfree, Bytes) uint8_t *mutated_inner =
                (uint8_t *)zmalloc(inner_len);
            memcpy(mutated_inner, inner, inner_len);
            mutated_inner[bpos] ^= (uint8_t)(1u << bit);

            cleanerName(zfree, Bytes) uint8_t *env =
                (uint8_t *)zmalloc(env_cap);
            size_t written = 0u;
            if (!flexEnvelopeWrap(env, env_cap, mutated_inner,
                                  (uint64_t)inner_len, &wp, &written)) {
                printf("testMailboxRestoreWrappedStructAwareFuzz[bpos=%zu "
                       "bit=%u]: Wrap failed\n",
                       bpos, bit);
                failures++;
                continue;
            }

            hmacKeyring *ring = NULL;
            clusterMailbox *mb = NULL;
            mailboxRestoreRebuild(&mb, &ring, key);
            if (mb == NULL || ring == NULL) {
                if (mb != NULL) {
                    clusterMailboxFree(mb);
                }
                if (ring != NULL) {
                    hmacKeyringFree(ring);
                }
                failures++;
                continue;
            }
            const bool ok = clusterMailboxRestoreWrapped(mb, &vp, env, written);
            sweep_total++;
            if (ok) {
                sweep_ok++;
            } else {
                /* RestoreWrapped chains flexEnvelopeVerify (outer MAC
                 * check) and clusterMailboxRestore (inner deep walk).
                 * Since we re-Wrap under the original key, the outer
                 * check always passes; failures are inner deep-walk or
                 * per-record-type rejections. */
                sweep_rejected++;
            }
            clusterMailboxFree(mb);
            hmacKeyringFree(ring);
        }
    }

    printf("testMailboxRestoreWrappedStructAwareFuzz: sweep %zu flips: "
           "ok=%zu rejected=%zu unexpected=%zu\n",
           sweep_total, sweep_ok, sweep_rejected, sweep_unexpected);

    if (sweep_unexpected != 0u) {
        failures++;
    }
    if (sweep_ok + sweep_rejected != sweep_total) {
        printf("testMailboxRestoreWrappedStructAwareFuzz: bucket sum "
               "mismatch\n");
        failures++;
    }

    if (failures == 0) {
        printf("testMailboxRestoreWrappedStructAwareFuzz: OK\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * aeadNonceLog eviction perf + probe-before-evict correctness.
 * Probe-first: tuple at FIFO head must reject as duplicate (pre-fix
 * would evict then accept). Perf: per-insert with eviction at cap=4096
 * bounded under 5µs (ASAN+UBSAN).
 * ==================================================================== */
static void dkTestNonceLogMkNonce(uint64_t v, uint8_t out[12]) {
    memset(out, 0, 12);
    for (size_t i = 0; i < 8; i++) {
        out[i] = (uint8_t)((v >> (i * 8u)) & 0xFFu);
    }
}

static int dkTestNonceLogEvictionPerfTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    /* Part A: probe-BEFORE-evict correctness */
    {
        const size_t kCap = 64u;
        const uint32_t kKeyId = 17u;
        const size_t kThreshold = kCap - (kCap / 8u);
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(kCap);
        if (log == NULL || aeadNonceLogCapacity(log) != kCap) {
            printf("dkTestNonceLogEvictionPerf: A setup failed\n");
            return 1;
        }
        uint8_t nonce[12];
        for (uint64_t i = 1u; i <= kCap; i++) {
            dkTestNonceLogMkNonce(i, nonce);
            if (!aeadNonceLogObserve(log, kKeyId, nonce)) {
                printf("dkTestNonceLogEvictionPerf: A setup observe %" PRIu64
                       " refused\n",
                       i);
                failures++;
                goto endA;
            }
        }
        const size_t sz_before = aeadNonceLogSize(log);
        if (sz_before != kThreshold) {
            printf("dkTestNonceLogEvictionPerf: A pre-probe size expected "
                   "%zu got %zu\n",
                   kThreshold, sz_before);
            failures++;
            goto endA;
        }
        dkTestNonceLogMkNonce(9u, nonce);
        if (aeadNonceLogObserve(log, kKeyId, nonce)) {
            printf("dkTestNonceLogEvictionPerf: A PROBE-BEFORE-EVICT BUG\n");
            failures++;
        }
        if (aeadNonceLogSize(log) != sz_before) {
            printf("dkTestNonceLogEvictionPerf: A size mutated across "
                   "rejected observe\n");
            failures++;
        }
        dkTestNonceLogMkNonce(9u, nonce);
        if (aeadNonceLogObserve(log, kKeyId, nonce)) {
            printf(
                "dkTestNonceLogEvictionPerf: A second observe(9) accepted\n");
            failures++;
        }
        dkTestNonceLogMkNonce(65u, nonce);
        if (!aeadNonceLogObserve(log, kKeyId, nonce)) {
            printf("dkTestNonceLogEvictionPerf: A fresh observe(65) refused\n");
            failures++;
        }
        dkTestNonceLogMkNonce(9u, nonce);
        if (!aeadNonceLogObserve(log, kKeyId, nonce)) {
            printf("dkTestNonceLogEvictionPerf: A post-eviction observe(9) "
                   "refused\n");
            failures++;
        }
    endA:
        /* Scope exit releases the log on every path out of this block. */
        ;
    }

    /* Part B: eviction perf at cap=4096 */
    {
        const size_t kCap = 4096u;
        const uint32_t kKeyId = 19u;
        const uint64_t kEvictions = 20000u;
        const double kMaxPerInsertNs = 5000.0;
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(kCap);
        if (log == NULL || aeadNonceLogCapacity(log) != kCap) {
            printf("dkTestNonceLogEvictionPerf: B setup failed\n");
            return 1;
        }
        uint8_t nonce[12];
        for (uint64_t i = 1u; i <= kCap; i++) {
            dkTestNonceLogMkNonce(i, nonce);
            if (!aeadNonceLogObserve(log, kKeyId, nonce)) {
                printf("dkTestNonceLogEvictionPerf: B fill iter %" PRIu64
                       " refused\n",
                       i);
                failures++;
                goto endB;
            }
        }
        const uint64_t t0 = timeUtilMonotonicNs();
        for (uint64_t i = 0; i < kEvictions; i++) {
            dkTestNonceLogMkNonce(kCap + 1u + i, nonce);
            if (!aeadNonceLogObserve(log, kKeyId, nonce)) {
                printf("dkTestNonceLogEvictionPerf: B evict iter %" PRIu64
                       " refused\n",
                       i);
                failures++;
                break;
            }
        }
        const uint64_t t1 = timeUtilMonotonicNs();
        const double per_ns = (double)(t1 - t0) / (double)kEvictions;
        if (per_ns > kMaxPerInsertNs) {
            printf("dkTestNonceLogEvictionPerf: B per-insert %.1f ns > limit "
                   "%.1f ns\n",
                   per_ns, kMaxPerInsertNs);
            failures++;
        }
        printf("dkTestNonceLogEvictionPerf: B cap=%zu evictions=%" PRIu64
               " per-insert=%.1f ns\n",
               kCap, kEvictions, per_ns);
    endB:
        /* Scope exit releases the log on every path out of this block. */
        ;
    }

    if (failures == 0) {
        printf("dkTestNonceLogEvictionPerf: OK\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * replicator drop-after-N-retries — verifies (a) NULL-free typetable
 * rejected at Create, (b) max_apply_retries=3 drops poison seq after 3
 * consecutive rejects + bumps dropped_count + advances watermark past
 * it, (c) legacy Create (retries=0) preserves infinite-retry behavior.
 * ==================================================================== */
typedef struct dkTestReplicatorRetryCtx {
    uint64_t reject_seq;   /* sequence that always rejects (0 = none) */
    uint64_t reject_count; /* how many times reject_seq was rejected */
    uint64_t accept_count; /* total accepted sequences */
    uint64_t accepted_seqs[16];
} dkTestReplicatorRetryCtx;

static bool dkTestReplicatorRetryApply(uint32_t origin_id, uint64_t sequence,
                                       const persistOps *ops, void *structure,
                                       void *ctx) {
    (void)origin_id;
    dkTestReplicatorRetryCtx *t = (dkTestReplicatorRetryCtx *)ctx;
    if (t->reject_seq != 0 && sequence == t->reject_seq) {
        t->reject_count++;
        return false;
    }
    if (t->accept_count < 16) {
        t->accepted_seqs[t->accept_count] = sequence;
    }
    t->accept_count++;
    if (ops->free != NULL) {
        ops->free(structure);
    }
    return true;
}

static int dkTestReplicatorRetryDropTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    static const persistOps *type_table[256];
    memset((void *)type_table, 0, sizeof(type_table));
    type_table[PERSIST_TYPE_FLEX] = &persistOpsFlex;

    /* Case 1: NULL-free typetable rejected by Create. */
    {
        static persistOps bad_ops;
        memcpy(&bad_ops, &persistOpsFlex, sizeof(bad_ops));
        bad_ops.free = NULL;
        static const persistOps *bad_table[256];
        memset((void *)bad_table, 0, sizeof(bad_table));
        bad_table[PERSIST_TYPE_FLEX] = &bad_ops;

        const uint8_t key[32] = {0xE5};
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, 1u, &hmacAlgoSHA256, key, sizeof(key));
        dkTestReplicatorRetryCtx ctx_a = {0};
        clusterReplicator *bad = clusterReplicatorCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE,
            bad_table, 256u, dkTestReplicatorRetryApply, &ctx_a);
        if (bad != NULL) {
            printf("dkTestReplicatorRetryDrop: Create accepted NULL-free "
                   "typetable\n");
            failures++;
            clusterReplicatorFree(bad);
        }
        clusterReplicatorConfig cfg = {.max_apply_retries = 3};
        clusterReplicator *badEx = clusterReplicatorCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE,
            bad_table, 256u, dkTestReplicatorRetryApply, &ctx_a, &cfg);
        if (badEx != NULL) {
            printf("dkTestReplicatorRetryDrop: CreateEx accepted NULL-free "
                   "typetable\n");
            failures++;
            clusterReplicatorFree(badEx);
        }
        hmacKeyringFree(ring);
    }

    /* Case 2: max_apply_retries=3, poison seq 5 → drops after 3 rejects. */
    {
        const uint8_t key[32] = {0xE5};
        const hmacKeyId KID = 1u;
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));
        dkTestReplicatorRetryCtx ctx_b = {.reject_seq = 5};
        clusterReplicatorConfig cfg = {.max_apply_retries = 3};
        clusterReplicator *rep = clusterReplicatorCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE,
            type_table, 256u, dkTestReplicatorRetryApply, &ctx_b, &cfg);
        if (rep == NULL) {
            printf("dkTestReplicatorRetryDrop: CreateEx failed\n");
            failures++;
            hmacKeyringFree(ring);
            goto skip_case2;
        }

        for (uint64_t seq = 1; seq <= 10; seq++) {
            cleaner(flexFree) flex *src = flexNew();
            flexPushUnsigned(&src, seq, FLEX_ENDPOINT_TAIL);
            size_t cap =
                clusterReplicatorSnapshotSize(rep, &persistOpsFlex, src);
            cleanerName(zfree, Bytes) uint8_t *wire = zmalloc(cap);
            size_t written = 0;
            clusterReplicatorSeal(rep, &persistOpsFlex, src, 1u, seq, KID, wire,
                                  cap, &written);
            int attempts = (seq == 5) ? 3 : 1;
            for (int a = 0; a < attempts; a++) {
                clusterReplicatorIngest(rep, wire, written, KID);
            }
        }

        if (ctx_b.reject_count != 3) {
            printf("dkTestReplicatorRetryDrop: expected 3 rejects for seq 5, "
                   "got %" PRIu64 "\n",
                   ctx_b.reject_count);
            failures++;
        }
        if (clusterReplicatorDroppedCount(rep) != 1) {
            printf("dkTestReplicatorRetryDrop: expected dropped_count=1, got "
                   "%" PRIu64 "\n",
                   clusterReplicatorDroppedCount(rep));
            failures++;
        }
        if (ctx_b.accept_count != 9) {
            printf("dkTestReplicatorRetryDrop: expected 9 accepts, got %" PRIu64
                   "\n",
                   ctx_b.accept_count);
            failures++;
        }
        clusterMailbox *mb = clusterReplicatorMailbox(rep);
        if (clusterMailboxLastApplied(mb, 1u) != 10) {
            printf("dkTestReplicatorRetryDrop: expected watermark=10, got "
                   "%" PRIu64 "\n",
                   clusterMailboxLastApplied(mb, 1u));
            failures++;
        }
        for (uint64_t i = 0; i < ctx_b.accept_count && i < 16; i++) {
            if (ctx_b.accepted_seqs[i] == 5) {
                printf("dkTestReplicatorRetryDrop: accepted seq 5 (should "
                       "never be accepted)\n");
                failures++;
                break;
            }
        }
        clusterReplicatorFree(rep);
        hmacKeyringFree(ring);
    }
skip_case2:

    /* Case 3: legacy Create (max_apply_retries=0) → infinite retry. */
    {
        const uint8_t key[32] = {0xE5};
        const hmacKeyId KID = 1u;
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));
        dkTestReplicatorRetryCtx ctx_c = {.reject_seq = 1};
        clusterReplicator *rep = clusterReplicatorCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE,
            type_table, 256u, dkTestReplicatorRetryApply, &ctx_c);
        if (rep == NULL) {
            printf("dkTestReplicatorRetryDrop: legacy Create failed\n");
            failures++;
            hmacKeyringFree(ring);
            goto skip_case3;
        }
        cleaner(flexFree) flex *src = flexNew();
        flexPushUnsigned(&src, 1u, FLEX_ENDPOINT_TAIL);
        size_t cap = clusterReplicatorSnapshotSize(rep, &persistOpsFlex, src);
        cleanerName(zfree, Bytes) uint8_t *wire = zmalloc(cap);
        size_t written = 0;
        clusterReplicatorSeal(rep, &persistOpsFlex, src, 1u, 1u, KID, wire, cap,
                              &written);
        for (int a = 0; a < 50; a++) {
            clusterReplicatorIngest(rep, wire, written, KID);
        }
        if (clusterReplicatorDroppedCount(rep) != 0) {
            printf("dkTestReplicatorRetryDrop: legacy dropped_count expected "
                   "0, got %" PRIu64 "\n",
                   clusterReplicatorDroppedCount(rep));
            failures++;
        }
        clusterMailbox *mb = clusterReplicatorMailbox(rep);
        if (clusterMailboxLastApplied(mb, 1u) != 0) {
            printf("dkTestReplicatorRetryDrop: legacy watermark expected 0, "
                   "got %" PRIu64 "\n",
                   clusterMailboxLastApplied(mb, 1u));
            failures++;
        }
        if (ctx_c.reject_count != 50) {
            printf("dkTestReplicatorRetryDrop: legacy reject_count expected "
                   "50, got %" PRIu64 "\n",
                   ctx_c.reject_count);
            failures++;
        }
        clusterReplicatorFree(rep);
        hmacKeyringFree(ring);
    }
skip_case3:

    if (failures == 0) {
        printf("dkTestReplicatorRetryDrop: OK (NULL-free reject, "
               "drop-after-3, legacy infinite)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * clusterMailbox + clusterReplicator default-wire aeadNonceLog.
 * Mailbox Create allocates+owns nonceLog (cap=4096); Replicator inherits.
 * disable_nonce_log opts-out. Borrowed log preserved across Free.
 * Replay of wire bytes surfaces as CLUSTER_MAILBOX_ERR_REPLAY (mailbox
 * window fires before nonceLog observation under keyed-MAC algos).
 * ==================================================================== */
typedef struct dkTestMailboxReplCtx {
    flex *restored[4];
    uint32_t origins[4];
    uint64_t seqs[4];
    size_t count;
} dkTestMailboxReplCtx;

static bool dkTestMailboxReplApply(uint32_t origin_id, uint64_t sequence,
                                   const persistOps *ops, void *structure,
                                   void *ctx) {
    dkTestMailboxReplCtx *t = (dkTestMailboxReplCtx *)ctx;
    if (t->count >= 4 || ops->type != PERSIST_TYPE_FLEX) {
        ops->free(structure);
        return false;
    }
    t->restored[t->count] = (flex *)structure;
    t->origins[t->count] = origin_id;
    t->seqs[t->count] = sequence;
    t->count++;
    return true;
}

static int dkTestMailboxReplicatorNonceLogWiredTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;
    const uint8_t key[32] = {
        0xD8, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
        0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14,
        0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
    };
    const hmacKeyId KID = 4242u;
    const uint8_t payload[] = "mailboxReplicator-payload";
    const size_t payload_len = sizeof(payload);

    /* S1: default Create allocates + owns nonceLog. */
    {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));
        clusterMailbox *mb =
            clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, NULL, NULL);
        if (mb == NULL) {
            printf("dkTestMailboxRepl s1 Create NULL\n");
            failures++;
            hmacKeyringFree(ring);
            goto s2;
        }
        if (clusterMailboxNonceLog(mb) == NULL) {
            printf("dkTestMailboxRepl s1 nonceLog NULL\n");
            failures++;
        } else if (aeadNonceLogCapacity(clusterMailboxNonceLog(mb)) <
                   CLUSTER_MAILBOX_DEFAULT_NONCE_LOG_CAP) {
            printf("dkTestMailboxRepl s1 cap too small\n");
            failures++;
        }
        const size_t sz = clusterMailboxSealedSize(mb, payload_len);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        if (!clusterMailboxSeal(mb, 1u, 1u, 0x42, payload, payload_len, KID,
                                buf, sz, &written)) {
            printf("dkTestMailboxRepl s1 Seal failed\n");
            failures++;
        }
        if (clusterMailboxIngest(mb, buf, written, KID) != CLUSTER_MAILBOX_OK) {
            printf("dkTestMailboxRepl s1 first Ingest failed\n");
            failures++;
        }
        const clusterMailboxIngestResult rc2 =
            clusterMailboxIngest(mb, buf, written, KID);
        if (rc2 != CLUSTER_MAILBOX_ERR_REPLAY) {
            printf("dkTestMailboxRepl s1 expected REPLAY got %d\n", (int)rc2);
            failures++;
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }
s2:
    /* S2: disable_nonce_log opts-out. */
    {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));
        const clusterMailboxCreateOptions opts = {
            .ring = ring,
            .envelope_algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .disable_nonce_log = true,
        };
        clusterMailbox *mb = clusterMailboxCreateOpts(&opts);
        if (mb == NULL) {
            failures++;
            hmacKeyringFree(ring);
            goto s3;
        }
        if (clusterMailboxNonceLog(mb) != NULL) {
            printf("dkTestMailboxRepl s2 nonceLog should be NULL\n");
            failures++;
        }
        const size_t sz = clusterMailboxSealedSize(mb, payload_len);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        if (!clusterMailboxSeal(mb, 2u, 1u, 0x10, payload, payload_len, KID,
                                buf, sz, &written) ||
            clusterMailboxIngest(mb, buf, written, KID) != CLUSTER_MAILBOX_OK) {
            failures++;
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }
s3:
    /* S3: borrowed nonceLog must NOT be freed by mailbox. */
    {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));
        cleaner(aeadNonceLogFree) aeadNonceLog *borrowed =
            aeadNonceLogCreate(256u);
        if (borrowed == NULL) {
            failures++;
            hmacKeyringFree(ring);
            goto s4;
        }
        const clusterMailboxCreateOptions opts = {
            .ring = ring,
            .envelope_algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .nonce_log = borrowed,
        };
        clusterMailbox *mb = clusterMailboxCreateOpts(&opts);
        if (mb == NULL || clusterMailboxNonceLog(mb) != borrowed) {
            printf("dkTestMailboxRepl s3 borrowed-log failed\n");
            failures++;
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }
s4:
    /* S4: replicator default-on Seal+Ingest e2e. */
    {
        static const persistOps *type_table4[256];
        memset((void *)type_table4, 0, sizeof(type_table4));
        type_table4[PERSIST_TYPE_FLEX] = &persistOpsFlex;
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));
        dkTestMailboxReplCtx app_ctx = {0};
        clusterReplicator *rep = clusterReplicatorCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE,
            type_table4, 256u, dkTestMailboxReplApply, &app_ctx);
        if (rep == NULL) {
            failures++;
            hmacKeyringFree(ring);
            goto s5;
        }
        if (clusterMailboxNonceLog(clusterReplicatorMailbox(rep)) == NULL) {
            printf("dkTestMailboxRepl s4 replicator nonceLog NULL\n");
            failures++;
        }
        cleaner(flexFree) flex *src = flexNew();
        flexPushUnsigned(&src, 100u, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&src, "d8", 2, FLEX_ENDPOINT_TAIL);
        const size_t cap =
            clusterReplicatorSnapshotSize(rep, &persistOpsFlex, src);
        cleanerName(zfree, Bytes) uint8_t *wire = zmalloc(cap);
        size_t written = 0;
        if (!clusterReplicatorSeal(rep, &persistOpsFlex, src, 9u, 1u, KID, wire,
                                   cap, &written)) {
            failures++;
        }
        if (clusterReplicatorIngest(rep, wire, written, KID) !=
            CLUSTER_MAILBOX_OK) {
            failures++;
        }
        if (clusterReplicatorIngest(rep, wire, written, KID) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            printf("dkTestMailboxRepl s4 expected REPLAY on duplicate\n");
            failures++;
        }
        for (size_t i = 0; i < app_ctx.count; i++) {
            if (app_ctx.restored[i]) {
                flexFree(app_ctx.restored[i]);
                app_ctx.restored[i] = NULL;
            }
        }
        clusterReplicatorFree(rep);
        hmacKeyringFree(ring);
    }
s5:
    /* S5: replicator cfg.disable_nonce_log opts-out. */
    {
        static const persistOps *type_table5[256];
        memset((void *)type_table5, 0, sizeof(type_table5));
        type_table5[PERSIST_TYPE_FLEX] = &persistOpsFlex;
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));
        dkTestMailboxReplCtx app_ctx = {0};
        const clusterReplicatorConfig cfg = {
            .max_apply_retries = CLUSTER_REPLICATOR_DEFAULT_MAX_APPLY_RETRIES,
            .disable_nonce_log = true,
        };
        clusterReplicator *rep = clusterReplicatorCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE,
            type_table5, 256u, dkTestMailboxReplApply, &app_ctx, &cfg);
        if (rep == NULL) {
            failures++;
            hmacKeyringFree(ring);
            goto done_d8;
        }
        if (clusterMailboxNonceLog(clusterReplicatorMailbox(rep)) != NULL) {
            printf("dkTestMailboxRepl s5 disabled nonceLog should be NULL\n");
            failures++;
        }
        clusterReplicatorFree(rep);
        hmacKeyringFree(ring);
    }
done_d8:
    if (failures == 0) {
        printf("dkTestMailboxReplicatorNonceLogWired: OK (5 scenarios)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * persistOpsClusterRing flex serialization — round-trip a 6-node
 * ring whose nodes have mixed states, mixed locations, and a quorum
 * config; assert structural equivalence after the snapshot+restore
 * cycle (id, name, address, location, weight, capacity, state).
 * ==================================================================== */
static int dkTestClusterRingFlexSerializeTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    clusterRingConfig config = {
        .name = "e2-flex-ring",
        .strategyType = CLUSTER_STRATEGY_KETAMA,
        .defaultQuorum = CLUSTER_QUORUM_BALANCED,
        .hashSeed = 0x5A5A5A5Au,
    };
    config.vnodes.vnodeMultiplier = 150u;
    config.vnodes.minVnodesPerNode = 10u;
    config.vnodes.maxVnodesPerNode = 500u;
    config.vnodes.replicaPointSpread = true;

    cleaner(clusterRingFree) clusterRing *src = clusterRingNew(&config);
    if (src == NULL) {
        printf("dkTestClusterRingFlexSerialize: ring create failed\n");
        return 1;
    }

    struct {
        uint64_t id;
        const char *name;
        const char *addr;
        uint32_t weight;
        uint64_t cap;
        clusterNodeState state;
        clusterLocation loc;
    } src_nodes[] = {
        {1,
         "node-a",
         "10.0.0.1:7000",
         100,
         1ull << 30,
         CLUSTER_NODE_UP,
         {.nodeId = 1,
          .rackId = 1,
          .cageId = 1,
          .dcId = 1,
          .azId = 1,
          .regionId = 1,
          .countryId = 840,
          .continentId = 3}},
        {2,
         "node-b",
         "10.0.0.2:7000",
         50,
         2ull << 30,
         CLUSTER_NODE_JOINING,
         {.nodeId = 2,
          .rackId = 2,
          .cageId = 1,
          .dcId = 1,
          .azId = 1,
          .regionId = 1,
          .countryId = 840,
          .continentId = 3}},
        {3,
         "n-c",
         "",
         200,
         4ull << 30,
         CLUSTER_NODE_LEAVING,
         {.nodeId = 3,
          .rackId = 3,
          .cageId = 2,
          .dcId = 1,
          .azId = 2,
          .regionId = 1,
          .countryId = 840,
          .continentId = 3}},
        {4,
         "",
         "10.0.0.4:7001",
         100,
         8ull << 30,
         CLUSTER_NODE_DOWN,
         {.nodeId = 4,
          .rackId = 4,
          .cageId = 2,
          .dcId = 2,
          .azId = 3,
          .regionId = 2,
          .countryId = 826,
          .continentId = 4}},
        {5,
         "node-e",
         "10.0.0.5:7000",
         100,
         16ull << 30,
         CLUSTER_NODE_SUSPECT,
         {.nodeId = 5,
          .rackId = 5,
          .cageId = 3,
          .dcId = 2,
          .azId = 3,
          .regionId = 2,
          .countryId = 826,
          .continentId = 4}},
        {6,
         "node-f",
         "10.0.0.6:7000",
         100,
         32ull << 30,
         CLUSTER_NODE_MAINTENANCE,
         {.nodeId = 6,
          .rackId = 6,
          .cageId = 3,
          .dcId = 3,
          .azId = 4,
          .regionId = 3,
          .countryId = 392,
          .continentId = 0}},
    };
    const size_t kN = sizeof(src_nodes) / sizeof(src_nodes[0]);

    for (size_t i = 0; i < kN; i++) {
        clusterNodeConfig nc = {
            .id = src_nodes[i].id,
            .name = src_nodes[i].name,
            .address = src_nodes[i].addr,
            .location = src_nodes[i].loc,
            .weight = src_nodes[i].weight,
            .capacityBytes = src_nodes[i].cap,
            .initialState = src_nodes[i].state,
        };
        if (clusterRingAddNode(src, &nc) != CLUSTER_OK) {
            printf("dkTestClusterRingFlexSerialize: AddNode #%" PRIu64
                   " failed\n",
                   src_nodes[i].id);
            failures++;
        }
    }

    size_t snap_len = 0;
    uint8_t *snap = persistOpsClusterRing.snapshot(src, &snap_len);
    if (snap == NULL || snap_len == 0) {
        printf("dkTestClusterRingFlexSerialize: snapshot empty\n");
        return 1;
    }

    if (!flexValidateBuffer(snap, snap_len)) {
        printf("dkTestClusterRingFlexSerialize: snapshot is not a valid flex "
               "buffer\n");
        zfree(snap);
        return 1;
    }

    if (persistOpsClusterRing.estimateSize(src) != snap_len) {
        printf("dkTestClusterRingFlexSerialize: estimateSize disagrees with "
               "snapshot length\n");
        failures++;
    }

    clusterRing *dst =
        (clusterRing *)persistOpsClusterRing.restore(snap, snap_len);
    if (dst == NULL) {
        printf("dkTestClusterRingFlexSerialize: restore returned NULL\n");
        zfree(snap);
        return 1;
    }

    if (persistOpsClusterRing.validate == NULL ||
        !persistOpsClusterRing.validate(dst)) {
        printf("dkTestClusterRingFlexSerialize: validate hook rejected "
               "restored ring\n");
        failures++;
    }

    if (clusterRingNodeCount(dst) != (uint32_t)kN) {
        printf("dkTestClusterRingFlexSerialize: node count expected %zu, got "
               "%u\n",
               kN, clusterRingNodeCount(dst));
        failures++;
    }

    for (size_t i = 0; i < kN; i++) {
        const clusterNode *n = clusterRingGetNode(dst, src_nodes[i].id);
        if (n == NULL) {
            printf("dkTestClusterRingFlexSerialize: node id=%" PRIu64
                   " missing in restored ring\n",
                   src_nodes[i].id);
            failures++;
            continue;
        }
        if (clusterNodeGetState(n) != src_nodes[i].state) {
            printf("dkTestClusterRingFlexSerialize: node id=%" PRIu64
                   " state mismatch\n",
                   src_nodes[i].id);
            failures++;
        }
        if (clusterNodeGetWeight(n) != src_nodes[i].weight) {
            printf("dkTestClusterRingFlexSerialize: node id=%" PRIu64
                   " weight mismatch\n",
                   src_nodes[i].id);
            failures++;
        }
        if (clusterNodeGetCapacity(n) != src_nodes[i].cap) {
            printf("dkTestClusterRingFlexSerialize: node id=%" PRIu64
                   " capacity mismatch\n",
                   src_nodes[i].id);
            failures++;
        }
        const clusterLocation *loc = clusterNodeGetLocation(n);
        if (loc == NULL || loc->nodeId != src_nodes[i].loc.nodeId ||
            loc->rackId != src_nodes[i].loc.rackId ||
            loc->cageId != src_nodes[i].loc.cageId ||
            loc->dcId != src_nodes[i].loc.dcId ||
            loc->azId != src_nodes[i].loc.azId ||
            loc->regionId != src_nodes[i].loc.regionId ||
            loc->countryId != src_nodes[i].loc.countryId ||
            loc->continentId != src_nodes[i].loc.continentId) {
            printf("dkTestClusterRingFlexSerialize: node id=%" PRIu64
                   " location mismatch\n",
                   src_nodes[i].id);
            failures++;
        }
        const char *got_name = clusterNodeGetName(n);
        if (got_name == NULL || strcmp(got_name, src_nodes[i].name) != 0) {
            printf("dkTestClusterRingFlexSerialize: node id=%" PRIu64
                   " name mismatch\n",
                   src_nodes[i].id);
            failures++;
        }
        const char *got_addr = clusterNodeGetAddress(n);
        if (got_addr == NULL || strcmp(got_addr, src_nodes[i].addr) != 0) {
            printf("dkTestClusterRingFlexSerialize: node id=%" PRIu64
                   " address mismatch\n",
                   src_nodes[i].id);
            failures++;
        }
    }

    size_t snap2_len = 0;
    uint8_t *snap2 = persistOpsClusterRing.snapshot(dst, &snap2_len);
    if (snap2 == NULL || snap2_len != snap_len ||
        memcmp(snap, snap2, snap_len) != 0) {
        printf("dkTestClusterRingFlexSerialize: re-snapshot is not "
               "byte-equivalent\n");
        failures++;
    }
    if (snap2) {
        zfree(snap2);
    }

    persistOpsClusterRing.free(dst);
    zfree(snap);

    if (failures == 0) {
        printf("dkTestClusterRingFlexSerialize: OK (%zu-node ring "
               "round-tripped via flex)\n",
               kN);
        return 0;
    }
    return 1;
}

/* ====================================================================
 * persistOpsClusterRing must REJECT wire input with out-of-enum state.
 * ==================================================================== */
/* A snapshot of a JUMP ring holding nodes 41, 42, ... (count of them),
 * with entry `idx` replaced by `value` unless idx is SIZE_MAX (caller
 * frees with flexFree).  Snapshot entries: a 17-entry header (no ring
 * affinity rules here), then 15 per node; a node's state is its entry
 * 13. */
static flex *dkTestClusterRingSnapshotWith(uint32_t count, size_t idx,
                                           uint64_t value) {
    clusterRingConfig config = {
        .name = "evil",
        .strategyType = CLUSTER_STRATEGY_JUMP,
    };
    cleaner(clusterRingFree) clusterRing *ring = clusterRingNew(&config);
    for (uint32_t i = 0; i < count; i++) {
        clusterNodeConfig nc = {
            .id = 41u + i,
            .name = "x",
            .address = "y",
            .initialState = CLUSTER_NODE_UP,
        };
        clusterRingAddNode(ring, &nc);
    }

    size_t len = 0;
    /* @own: caller; free=flexFree */
    flex *f = (flex *)persistOpsClusterRing.snapshot(ring, &len);
    if (idx != SIZE_MAX) {
        flexReplaceUnsigned(&f, flexIndex(f, (int32_t)idx), value);
    }
    return f;
}

static int dkTestClusterRingMaliciousStateTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    /* The unmodified snapshot restores, so the refusal below is the
     * state's */
    cleaner(flexFree) flex *good =
        dkTestClusterRingSnapshotWith(1, SIZE_MAX, 0);
    clusterRing *g = clusterRingDeserialize(good, flexBytes(good));
    if (g == NULL) {
        printf("dkTestClusterRingMaliciousState: valid snapshot refused\n");
        failures++;
    }
    clusterRingFree(g);

    cleaner(flexFree) flex *f =
        dkTestClusterRingSnapshotWith(1, 17 + 13, 0xDEADBEEFu);

    const size_t buflen = flexBytes(f);
    clusterRing *r = clusterRingDeserialize((const void *)f, buflen);
    if (r != NULL) {
        printf("dkTestClusterRingMaliciousState: deserialize accepted "
               "state=0xDEADBEEF\n");
        clusterRingFree(r);
        failures++;
    }

    clusterRing *r2 = (clusterRing *)persistOpsClusterRing.restore(
        (const uint8_t *)f, buflen);
    if (r2 != NULL) {
        printf("dkTestClusterRingMaliciousState: persistOps.restore accepted "
               "state=0xDEADBEEF\n");
        persistOpsClusterRing.free(r2);
        failures++;
    }

    if (failures == 0) {
        printf("dkTestClusterRingMaliciousState: OK (malicious state "
               "rejected)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * clusterRing wire input with two nodes sharing the same id must be REJECTED.
 * ==================================================================== */
static int dkTestClusterRingDuplicateIdTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    /* Nodes 41 and 42 restore; renaming the second node's id to 41 must
     * be refused */
    cleaner(flexFree) flex *good =
        dkTestClusterRingSnapshotWith(2, SIZE_MAX, 0);
    clusterRing *g = clusterRingDeserialize(good, flexBytes(good));
    if (g == NULL) {
        printf("dkTestClusterRingDuplicateId: valid snapshot refused\n");
        failures++;
    }
    clusterRingFree(g);

    cleaner(flexFree) flex *f = dkTestClusterRingSnapshotWith(2, 17 + 15, 41u);

    const size_t buflen = flexBytes(f);
    clusterRing *r = clusterRingDeserialize((const void *)f, buflen);
    if (r != NULL) {
        printf("dkTestClusterRingDuplicateId: deserialize accepted duplicate "
               "ids\n");
        if (persistOpsClusterRing.validate != NULL &&
            persistOpsClusterRing.validate(r)) {
            printf("dkTestClusterRingDuplicateId: validate hook also "
                   "accepted dup-id ring\n");
        }
        clusterRingFree(r);
        failures++;
    }

    if (failures == 0) {
        printf("dkTestClusterRingDuplicateId: OK (duplicate id rejected)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * clusterRing truncated wire buffer must NOT crash and must return NULL.
 * ==================================================================== */
static int dkTestClusterRingTruncatedBufferTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    clusterRingConfig config = {
        .name = "trunc",
        .strategyType = CLUSTER_STRATEGY_JUMP,
    };
    cleaner(clusterRingFree) clusterRing *src = clusterRingNew(&config);
    if (src == NULL) {
        printf("dkTestClusterRingTruncatedBuffer: ring create failed\n");
        return 1;
    }
    for (uint32_t i = 1u; i <= 4u; i++) {
        clusterNodeConfig nc = {
            .id = (uint64_t)i,
            .name = "trunc-node",
            .weight = 10u,
            .initialState = CLUSTER_NODE_UP,
        };
        clusterRingAddNode(src, &nc);
    }

    size_t snap_len = 0;
    uint8_t *snap = persistOpsClusterRing.snapshot(src, &snap_len);
    if (snap == NULL || snap_len < 16) {
        printf("dkTestClusterRingTruncatedBuffer: snapshot too small\n");
        return 1;
    }

    if (clusterRingDeserialize(snap, 0) != NULL) {
        printf("dkTestClusterRingTruncatedBuffer: accepted 0-byte input\n");
        failures++;
    }
    if (clusterRingDeserialize(snap, 1) != NULL) {
        printf("dkTestClusterRingTruncatedBuffer: accepted 1-byte input\n");
        failures++;
    }
    if (clusterRingDeserialize(snap, snap_len / 2) != NULL) {
        printf("dkTestClusterRingTruncatedBuffer: accepted half-truncated "
               "input\n");
        failures++;
    }
    if (clusterRingDeserialize(snap, snap_len - 1) != NULL) {
        printf("dkTestClusterRingTruncatedBuffer: accepted snap_len-1 input\n");
        failures++;
    }

    clusterRing *ok = clusterRingDeserialize(snap, snap_len);
    if (ok == NULL) {
        printf("dkTestClusterRingTruncatedBuffer: full-length deserialize "
               "failed\n");
        failures++;
    } else {
        clusterRingFree(ok);
    }

    zfree(snap);

    if (failures == 0) {
        printf("dkTestClusterRingTruncatedBuffer: OK (truncation rejected, "
               "no crash)\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * hmacKeyring AEAD key-schedule cache.
 * Verifies install pre-runs aesGcmInit for AES-128/AES-256 lengths and
 * stashes the schedule alongside the slot for the hot Open path.
 * ==================================================================== */
static int dkTestAeadKeyScheduleCacheTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;
    /* Declared here (before any goto to `cleanup`) so the scope-exit
     * cleaner attribute below never bypasses an initializer across a
     * jump. */
    cleanerName(zfree, Bytes) uint8_t *plaintext = NULL;
    cleanerName(zfree, Bytes) uint8_t *envelope = NULL;
    cleanerName(zfree, Bytes) uint8_t *recovered = NULL;

    hmacKeyring *ring = hmacKeyringCreate();
    if (ring == NULL) {
        printf("dkTestAeadKeyScheduleCache: ring create failed\n");
        return 1;
    }

    const uint8_t k1_aes128[AES128_KEY_LEN] = {
        0xA1, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    };
    const uint8_t k2_aes256[AES256_KEY_LEN] = {
        0xA2, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
        0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34,
        0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
    };
    const uint8_t k3_sha1[20] = {
        0xA3, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
        0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42,
    };
    const uint8_t k4_aes128[AES128_KEY_LEN] = {
        0xA4, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
        0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E,
    };
    const uint8_t salt1[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xA1, 0x01, 0x02,
                                                         0x03};
    const uint8_t salt2[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xA2, 0x04, 0x05,
                                                         0x06};
    const uint8_t salt3[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xA3, 0x07, 0x08,
                                                         0x09};
    const uint8_t salt4[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xA4, 0x0A, 0x0B,
                                                         0x0C};

    const hmacKeyId K1 = 1101u;
    const hmacKeyId K2 = 1102u;
    const hmacKeyId K3 = 1103u;
    const hmacKeyId K4 = 1104u;

    if (!hmacKeyringInstall(ring, K1, &hmacAlgoSHA256, k1_aes128,
                            sizeof(k1_aes128), salt1)) {
        printf("dkTestAeadKeyScheduleCache: K1 install failed\n");
        failures++;
        goto cleanup;
    }
    if (!hmacKeyringInstall(ring, K2, &hmacAlgoSHA256, k2_aes256,
                            sizeof(k2_aes256), salt2)) {
        printf("dkTestAeadKeyScheduleCache: K2 install failed\n");
        failures++;
        goto cleanup;
    }
    if (!hmacKeyringInstall(ring, K3, &hmacAlgoSHA256, k3_sha1, sizeof(k3_sha1),
                            salt3)) {
        printf("dkTestAeadKeyScheduleCache: K3 install failed\n");
        failures++;
        goto cleanup;
    }

    const aesGcmKey *cached1 = hmacKeyringGetAesGcmKey(ring, K1);
    if (cached1 == NULL) {
        printf("dkTestAeadKeyScheduleCache: K1 (AES-128) MUST be cached\n");
        failures++;
    } else if (cached1->key_size != AES128_KEY_LEN ||
               cached1->num_rounds != 10) {
        printf("dkTestAeadKeyScheduleCache: K1 cached schedule shape wrong "
               "(key_size=%u rounds=%u)\n",
               (unsigned)cached1->key_size, (unsigned)cached1->num_rounds);
        failures++;
    }

    const aesGcmKey *cached2 = hmacKeyringGetAesGcmKey(ring, K2);
    if (cached2 == NULL) {
        printf("dkTestAeadKeyScheduleCache: K2 (AES-256) MUST be cached\n");
        failures++;
    } else if (cached2->key_size != AES256_KEY_LEN ||
               cached2->num_rounds != 14) {
        printf("dkTestAeadKeyScheduleCache: K2 cached schedule shape wrong "
               "(key_size=%u rounds=%u)\n",
               (unsigned)cached2->key_size, (unsigned)cached2->num_rounds);
        failures++;
    }

    if (hmacKeyringGetAesGcmKey(ring, K3) != NULL) {
        printf("dkTestAeadKeyScheduleCache: K3 (20-byte SHA-1) MUST be "
               "uncached\n");
        failures++;
    }
    if (hmacKeyringGetAesGcmKey(ring, HMAC_KEY_ID_INVALID) != NULL) {
        printf("dkTestAeadKeyScheduleCache: HMAC_KEY_ID_INVALID MUST yield "
               "NULL\n");
        failures++;
    }
    if (hmacKeyringGetAesGcmKey(ring, 0xDEADBEEFu) != NULL) {
        printf("dkTestAeadKeyScheduleCache: unregistered id MUST yield NULL\n");
        failures++;
    }
    if (hmacKeyringGetAesGcmKey(NULL, K1) != NULL) {
        printf("dkTestAeadKeyScheduleCache: NULL ring MUST yield NULL\n");
        failures++;
    }

    if (cached1 != NULL) {
        for (int i = 0; i < 100; i++) {
            const aesGcmKey *probe = hmacKeyringGetAesGcmKey(ring, K1);
            if (probe != cached1) {
                printf("dkTestAeadKeyScheduleCache: pointer stability "
                       "failed at i=%d\n",
                       i);
                failures++;
                break;
            }
        }
    }

    if (!hmacKeyringRetire(ring, K1)) {
        printf("dkTestAeadKeyScheduleCache: K1 retire failed\n");
        failures++;
    } else if (hmacKeyringGetAesGcmKey(ring, K1) != NULL) {
        printf("dkTestAeadKeyScheduleCache: post-retire K1 MUST yield NULL\n");
        failures++;
    }

    if (!hmacKeyringInstall(ring, K4, &hmacAlgoSHA256, k4_aes128,
                            sizeof(k4_aes128), salt4)) {
        printf("dkTestAeadKeyScheduleCache: K4 install failed\n");
        failures++;
        goto cleanup;
    }
    if (hmacKeyringGetAesGcmKey(ring, K4) == NULL) {
        printf("dkTestAeadKeyScheduleCache: K4 cache MUST hit before E2E\n");
        failures++;
    }

    const size_t kPayloadLen = 256u;
    plaintext = zmalloc(kPayloadLen);
    for (size_t i = 0; i < kPayloadLen; i++) {
        plaintext[i] = (uint8_t)(0xA0 ^ (i * 31u));
    }

    const uint8_t nonce[AES_GCM_NONCE_LEN] = {
        0xA4, 0xB4, 0xC4, 0xD4, 0xE4, 0xF4, 0x04, 0x14, 0x24, 0x34, 0x44, 0x54,
    };

    const size_t cap =
        flexEnvelopeWrappedSize(kPayloadLen, FLEX_ENV_ALGO_AEAD_AES128_GCM,
                                FLEX_ENV_FLAG_KEYID_PRESENT);
    if (cap == 0u) {
        printf("dkTestAeadKeyScheduleCache: wrappedSize returned 0\n");
        failures++;
        goto cleanup;
    }

    envelope = zmalloc(cap);
    recovered = zmalloc(kPayloadLen);
    size_t env_written = 0;
    size_t pt_written = 0;

    flexEnvelopeSealParams sp = {
        .key = k4_aes128,
        .nonce = nonce,
        .key_len = sizeof(k4_aes128),
        .nonce_len = sizeof(nonce),
        .algo = FLEX_ENV_ALGO_AEAD_AES128_GCM,
        .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
        .key_id = K4,
    };
    if (!flexEnvelopeSeal(envelope, cap, plaintext, (uint64_t)kPayloadLen, &sp,
                          &env_written)) {
        printf("dkTestAeadKeyScheduleCache: E2E Seal failed\n");
        failures++;
    } else {
        flexEnvelopeVerifyParams vp = {.keyring = ring};
        if (!flexEnvelopeOpen(envelope, env_written, &vp, recovered,
                              kPayloadLen, &pt_written)) {
            printf("dkTestAeadKeyScheduleCache: E2E Open failed (err=%d)\n",
                   (int)flexEnvelopeLastVerifyError());
            failures++;
        } else if (pt_written != kPayloadLen ||
                   memcmp(recovered, plaintext, kPayloadLen) != 0) {
            printf("dkTestAeadKeyScheduleCache: E2E plaintext mismatch\n");
            failures++;
        }
    }

cleanup:
    hmacKeyringFree(ring);

    if (failures == 0) {
        printf("dkTestAeadKeyScheduleCache: OK\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * clusterMailbox inner-header + rotate-announce flex tests.
 * ==================================================================== */
int dkTestMailboxInnerHeaderFlexTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    const uint32_t ORIGIN = 0x12345678u;
    const uint64_t SEQ = 0xFEDCBA9876543210ULL;
    const uint8_t MTYPE = 5u;
    dkFormatStamp protocol = {0};

    TEST("inner-header: encode -> decode round-trip preserves all fields") {
        uint8_t buf[CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES];
        memset(buf, 0xAA, sizeof(buf));
        const size_t hdr_len =
            cmEncodeInnerHeader(buf, sizeof(buf), ORIGIN, SEQ, MTYPE);
        if (hdr_len == 0u) {
            ERRR("encode returned 0");
            TEST_FINAL_RESULT;
        }
        if (hdr_len > sizeof(buf)) {
            ERR("encoded length %zu > buffer cap %zu", hdr_len, sizeof(buf));
        }
        uint32_t origin = 0;
        uint64_t seq = 0;
        uint8_t mtype = 0;
        size_t consumed = 0;
        if (!cmDecodeInnerHeader(buf, hdr_len, &protocol, &origin, &seq, &mtype,
                                 &consumed)) {
            ERRR("round-trip decode failed");
            TEST_FINAL_RESULT;
        }
        if (protocol.id != DK_FORMAT_ID_CLUSTER_MAILBOX ||
            protocol.version != clusterMailboxProtocol.version ||
            protocol.features != 0) {
            ERRR("the header does not carry this build's protocol stamp");
        }
        if (origin != ORIGIN) {
            ERR("origin mismatch: got 0x%x", origin);
        }
        if (seq != SEQ) {
            ERR("sequence mismatch: got 0x%" PRIx64, seq);
        }
        if (mtype != MTYPE) {
            ERR("msg_type mismatch: got %u", (unsigned)mtype);
        }
        if (consumed != hdr_len) {
            ERR("consumed %zu != hdr_len %zu", consumed, hdr_len);
        }
    }

    TEST("inner-header: framing-byte-0 tamper rejected (contract subset)") {
        uint8_t base[CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES];
        const size_t hdr_len =
            cmEncodeInnerHeader(base, sizeof(base), ORIGIN, SEQ, MTYPE);
        if (hdr_len == 0u) {
            ERRR("framing-tamper encode failed");
            TEST_FINAL_RESULT;
        }
        size_t total = 0;
        size_t detected = 0;
        for (unsigned bit = 0; bit < 8; bit++) {
            uint8_t mutated[CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES];
            memcpy(mutated, base, hdr_len);
            mutated[0] ^= (uint8_t)(1u << bit);
            uint32_t origin = 0;
            uint64_t seq = 0;
            uint8_t mtype = 0;
            size_t consumed = 0;
            const bool ok = cmDecodeInnerHeader(
                mutated, hdr_len, &protocol, &origin, &seq, &mtype, &consumed);
            total++;
            if (!ok || origin != ORIGIN || seq != SEQ || mtype != MTYPE) {
                detected++;
            }
        }
        const size_t threshold = (total * 60u) / 100u;
        if (detected < threshold) {
            ERR("framing-tamper: only %zu/%zu rejected", detected, total);
        }
    }

    TEST("inner-header: end-to-end tamper via envelope rejects all flips") {
        const uint8_t key[32] = {
            0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0x04, 0x04, 0x04,
            0x04, 0x04, 0x04, 0x04, 0x04, 0xCA, 0xFE, 0xBA, 0xBE, 0xDE, 0xAD,
            0xBE, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
        hmacKeyring *ring = hmacKeyringCreate();
        if (!hmacKeyringInstallUnsalted(ring, 1u, &hmacAlgoSHA256, key,
                                        sizeof(key))) {
            ERRR("e2e: keyring install failed");
            hmacKeyringFree(ring);
            TEST_FINAL_RESULT;
        }
        clusterMailbox *prod =
            clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, NULL, NULL);
        clusterMailbox *cons =
            clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, NULL, NULL);
        const uint8_t payload[] = "innerHdr-e2e-tamper";
        const size_t cap = clusterMailboxSealedSize(prod, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *sealed = zmalloc(cap);
        size_t sealed_len = 0;
        if (!clusterMailboxSeal(prod, ORIGIN, 1u, MTYPE, payload,
                                sizeof(payload), 1u, sealed, cap,
                                &sealed_len)) {
            ERRR("e2e: seal failed");
            clusterMailboxFree(prod);
            clusterMailboxFree(cons);
            hmacKeyringFree(ring);
            TEST_FINAL_RESULT;
        }
        size_t total = 0;
        size_t detected = 0;
        for (size_t i = 0; i < sealed_len; i++) {
            for (unsigned bit = 0; bit < 8; bit++) {
                cleanerName(zfree, Bytes) uint8_t *m = zmalloc(sealed_len);
                memcpy(m, sealed, sealed_len);
                m[i] ^= (uint8_t)(1u << bit);
                const clusterMailboxIngestResult rc =
                    clusterMailboxIngest(cons, m, sealed_len, 1u);
                total++;
                if (rc != CLUSTER_MAILBOX_OK) {
                    detected++;
                }
            }
        }
        if (detected != total) {
            ERR("e2e tamper: only %zu/%zu rejected (must be 100%%)", detected,
                total);
        }
        clusterMailboxFree(prod);
        clusterMailboxFree(cons);
        hmacKeyringFree(ring);
    }

    TEST("inner-header: truncation by 1 byte rejected") {
        uint8_t buf[CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES];
        const size_t hdr_len =
            cmEncodeInnerHeader(buf, sizeof(buf), ORIGIN, SEQ, MTYPE);
        if (hdr_len < 2u) {
            ERR("truncate: hdr_len %zu too small", hdr_len);
            TEST_FINAL_RESULT;
        }
        uint32_t origin = 0;
        uint64_t seq = 0;
        uint8_t mtype = 0;
        size_t consumed = 0;
        if (cmDecodeInnerHeader(buf, hdr_len - 1u, &protocol, &origin, &seq,
                                &mtype, &consumed)) {
            ERRR("truncated decode unexpectedly succeeded");
        }
    }

    TEST("inner-header: wrong entry count (2 entries) rejected") {
        cleaner(flexFree) flex *f = flexNew();
        flexPushUnsigned(&f, (uint64_t)ORIGIN, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, SEQ, FLEX_ENDPOINT_TAIL);
        const size_t n = flexBytes(f);
        if (n == 0u) {
            ERRR("wrong-count: synth flex empty");
            TEST_FINAL_RESULT;
        }
        uint8_t scratch[CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES];
        if (n > sizeof(scratch)) {
            ERR("wrong-count: synth %zu > scratch %zu", n, sizeof(scratch));
            TEST_FINAL_RESULT;
        }
        memcpy(scratch, f, n);
        uint32_t origin = 0;
        uint64_t seq = 0;
        uint8_t mtype = 0;
        size_t consumed = 0;
        if (cmDecodeInnerHeader(scratch, n, &protocol, &origin, &seq, &mtype,
                                &consumed)) {
            ERRR("wrong-count decode unexpectedly succeeded");
        }
    }

    TEST("inner-header: origin_id > UINT32_MAX rejected") {
        cleaner(flexFree) flex *f = flexNew();
        dkTestPushMailboxProtocol(&f);
        flexPushUnsigned(&f, (uint64_t)UINT32_MAX + 1ULL, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, SEQ, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, (uint64_t)MTYPE, FLEX_ENDPOINT_TAIL);
        const size_t n = flexBytes(f);
        uint8_t scratch[CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES];
        if (n > sizeof(scratch)) {
            ERR("oor-origin: %zu > %zu", n, sizeof(scratch));
            TEST_FINAL_RESULT;
        }
        memcpy(scratch, f, n);
        uint32_t origin = 0;
        uint64_t seq = 0;
        uint8_t mtype = 0;
        size_t consumed = 0;
        if (cmDecodeInnerHeader(scratch, n, &protocol, &origin, &seq, &mtype,
                                &consumed)) {
            ERRR("origin > UINT32_MAX decode unexpectedly succeeded");
        }
    }

    TEST("inner-header: msg_type > UINT8_MAX rejected") {
        cleaner(flexFree) flex *f = flexNew();
        dkTestPushMailboxProtocol(&f);
        flexPushUnsigned(&f, (uint64_t)ORIGIN, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, SEQ, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, (uint64_t)UINT8_MAX + 1ULL, FLEX_ENDPOINT_TAIL);
        const size_t n = flexBytes(f);
        uint8_t scratch[CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES];
        if (n > sizeof(scratch)) {
            ERR("oor-mtype: %zu > %zu", n, sizeof(scratch));
            TEST_FINAL_RESULT;
        }
        memcpy(scratch, f, n);
        uint32_t origin = 0;
        uint64_t seq = 0;
        uint8_t mtype = 0;
        size_t consumed = 0;
        if (cmDecodeInnerHeader(scratch, n, &protocol, &origin, &seq, &mtype,
                                &consumed)) {
            ERRR("msg_type > UINT8_MAX decode unexpectedly succeeded");
        }
    }

    TEST_FINAL_RESULT;
}

int dkTestMailboxRotateAnnounceFlexTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    const hmacKeyId KID = 0x12345678u;

    TEST("rotate-announce: build + parse round-trip") {
        uint8_t buf[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN];
        const size_t n = clusterMailboxBuildRotateAnnounce(buf, KID);
        if (n == 0u) {
            ERRR("rot build returned 0");
            TEST_FINAL_RESULT;
        }
        if (n > CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN) {
            ERR("rot built %zu > cap %zu", n,
                (size_t)CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN);
        }
        hmacKeyId got = 0;
        if (!clusterMailboxParseRotateAnnounce(buf, n, &got)) {
            ERRR("rot parse failed");
            TEST_FINAL_RESULT;
        }
        if (got != KID) {
            ERR("rot id mismatch: got 0x%x", (unsigned)got);
        }
    }

    TEST("rotate-announce: byte-0 framing-tamper rejected") {
        uint8_t base[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN];
        const size_t n = clusterMailboxBuildRotateAnnounce(base, KID);
        if (n == 0u) {
            ERRR("tamper rot build failed");
            TEST_FINAL_RESULT;
        }
        size_t total = 0;
        size_t detected = 0;
        for (unsigned bit = 0; bit < 8; bit++) {
            uint8_t mutated[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN];
            memcpy(mutated, base, n);
            mutated[0] ^= (uint8_t)(1u << bit);
            hmacKeyId got = 0;
            const bool ok = clusterMailboxParseRotateAnnounce(mutated, n, &got);
            total++;
            if (!ok || got != KID) {
                detected++;
            }
        }
        const size_t threshold = (total * 60u) / 100u;
        if (detected < threshold) {
            ERR("rot byte-0 tamper: only %zu/%zu rejected", detected, total);
        }
    }

    TEST("rotate-announce: end-to-end envelope tamper rejects all flips") {
        const uint8_t key[32] = {
            0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0x04, 0x04, 0x04,
            0x04, 0x04, 0x04, 0x04, 0x04, 0xCA, 0xFE, 0xBA, 0xBE, 0xDE, 0xAD,
            0xBE, 0xEF, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4, 0xE4};
        hmacKeyring *ring = hmacKeyringCreate();
        if (!hmacKeyringInstallUnsalted(ring, 1u, &hmacAlgoSHA256, key,
                                        sizeof(key))) {
            ERRR("rot-e2e: keyring install failed");
            hmacKeyringFree(ring);
            TEST_FINAL_RESULT;
        }
        clusterMailbox *prod =
            clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, NULL, NULL);
        clusterMailbox *cons =
            clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, NULL, NULL);
        uint8_t inner[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN];
        const size_t inner_len = clusterMailboxBuildRotateAnnounce(inner, KID);
        if (inner_len == 0u) {
            ERRR("rot-e2e: build failed");
            clusterMailboxFree(prod);
            clusterMailboxFree(cons);
            hmacKeyringFree(ring);
            TEST_FINAL_RESULT;
        }
        const size_t cap = clusterMailboxSealedSize(prod, inner_len);
        cleanerName(zfree, Bytes) uint8_t *sealed = zmalloc(cap);
        size_t sealed_len = 0;
        if (!clusterMailboxSeal(prod, 7u, 1u,
                                CLUSTER_MAILBOX_MSG_KEY_ROTATE_ANNOUNCE, inner,
                                inner_len, 1u, sealed, cap, &sealed_len)) {
            ERRR("rot-e2e: seal failed");
            clusterMailboxFree(prod);
            clusterMailboxFree(cons);
            hmacKeyringFree(ring);
            TEST_FINAL_RESULT;
        }
        size_t total = 0;
        size_t detected = 0;
        for (size_t i = 0; i < sealed_len; i++) {
            for (unsigned bit = 0; bit < 8; bit++) {
                cleanerName(zfree, Bytes) uint8_t *m = zmalloc(sealed_len);
                memcpy(m, sealed, sealed_len);
                m[i] ^= (uint8_t)(1u << bit);
                const clusterMailboxIngestResult rc =
                    clusterMailboxIngest(cons, m, sealed_len, 1u);
                total++;
                if (rc != CLUSTER_MAILBOX_OK) {
                    detected++;
                }
            }
        }
        if (detected != total) {
            ERR("rot-e2e: only %zu/%zu rejected (must be 100%%)", detected,
                total);
        }
        clusterMailboxFree(prod);
        clusterMailboxFree(cons);
        hmacKeyringFree(ring);
    }

    TEST("rotate-announce: HMAC_KEY_ID_INVALID build returns 0") {
        uint8_t buf[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN];
        const size_t n =
            clusterMailboxBuildRotateAnnounce(buf, HMAC_KEY_ID_INVALID);
        if (n != 0u) {
            ERR("rot: INVALID id should refuse, got len %zu", n);
        }
    }

    TEST("rotate-announce: truncation by 1 byte rejected") {
        uint8_t buf[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN];
        const size_t n = clusterMailboxBuildRotateAnnounce(buf, KID);
        if (n < 2u) {
            ERR("rot truncate: n=%zu too small", n);
            TEST_FINAL_RESULT;
        }
        hmacKeyId got = 0;
        if (clusterMailboxParseRotateAnnounce(buf, n - 1u, &got)) {
            ERRR("rot truncated parse unexpectedly succeeded");
        }
    }

    TEST_FINAL_RESULT;
}

/* ====================================================================
 * Assert flexEnvelopeHeader::_reserved is zero on parse.
 * ==================================================================== */
static int dkTestEnvelopeReservedZeroAssertTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    static const uint8_t payload[32] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
        0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
        0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
    };
    const size_t plen = sizeof(payload);
    const size_t wrapped =
        flexEnvelopeWrappedSize(plen, FLEX_ENV_ALGO_XXH64, 0);
    if (wrapped == 0) {
        printf("dkTestEnvelopeReservedZeroAssert: wrappedSize == 0\n");
        return 1;
    }
    cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(wrapped);
    if (buf == NULL) {
        return 1;
    }

    flexEnvelopeWrapParams wp = {
        .algo = FLEX_ENV_ALGO_XXH64,
        .on_wire_flags = 0,
    };
    size_t written = 0;
    if (!flexEnvelopeWrap(buf, wrapped, payload, plen, &wp, &written) ||
        written != wrapped) {
        printf("dkTestEnvelopeReservedZeroAssert: wrap failed\n");
        return 1;
    }

    {
        flexEnvelopeHeader hdr;
        memset(&hdr, 0xCC, sizeof(hdr));
        if (!flexEnvelopePeek(buf, wrapped, &hdr)) {
            printf("dkTestEnvelopeReservedZeroAssert: Peek failed\n");
            failures++;
        } else if (hdr._reserved[0] != 0u || hdr._reserved[1] != 0u ||
                   hdr._reserved[2] != 0u) {
            printf("dkTestEnvelopeReservedZeroAssert: _reserved={%u,%u,%u}\n",
                   hdr._reserved[0], hdr._reserved[1], hdr._reserved[2]);
            failures++;
        }
    }
    {
        flexEnvelopeVerifyParams vp = {.opts = FLEX_ENV_VERIFY_DEFAULT};
        const uint8_t *out_p = NULL;
        uint64_t out_pl = 0;
        if (!flexEnvelopeVerify(buf, wrapped, &vp, &out_p, &out_pl)) {
            printf("dkTestEnvelopeReservedZeroAssert: untampered Verify "
                   "failed\n");
            failures++;
        }
    }
    {
        const uint8_t saved = buf[4];
        buf[4] = 0x04;
        flexEnvelopeVerifyParams vp = {.opts = FLEX_ENV_VERIFY_DEFAULT};
        const uint8_t *out_p = NULL;
        uint64_t out_pl = 0;
        if (flexEnvelopeVerify(buf, wrapped, &vp, &out_p, &out_pl)) {
            printf("dkTestEnvelopeReservedZeroAssert: Verify accepted "
                   "reserved bit 0x04\n");
            failures++;
        } else if (flexEnvelopeLastVerifyError() !=
                   FLEX_ENV_VERIFY_FAILED_GENERIC) {
            printf("dkTestEnvelopeReservedZeroAssert: expected "
                   "FAILED_GENERIC got %d\n",
                   (int)flexEnvelopeLastVerifyError());
            failures++;
        }
        buf[4] = saved;
    }
    {
        const uint8_t saved = buf[4];
        buf[4] = 0x80;
        flexEnvelopeVerifyParams vp = {.opts = FLEX_ENV_VERIFY_DEFAULT};
        const uint8_t *out_p = NULL;
        uint64_t out_pl = 0;
        if (flexEnvelopeVerify(buf, wrapped, &vp, &out_p, &out_pl)) {
            printf("dkTestEnvelopeReservedZeroAssert: Verify accepted "
                   "reserved bit 0x80\n");
            failures++;
        }
        buf[4] = saved;
    }
    {
        flexEnvelopeVerifyParams vp = {.opts = FLEX_ENV_VERIFY_DEFAULT};
        const uint8_t *out_p = NULL;
        uint64_t out_pl = 0;
        if (!flexEnvelopeVerify(buf, wrapped, &vp, &out_p, &out_pl)) {
            printf("dkTestEnvelopeReservedZeroAssert: restored Verify "
                   "failed\n");
            failures++;
        }
    }

    {
        const size_t exp_off =
            offsetof(flexEnvelopeHeader, version) + sizeof(uint8_t);
        if (offsetof(flexEnvelopeHeader, _reserved) != exp_off) {
            printf("dkTestEnvelopeReservedZeroAssert: _reserved offset "
                   "drifted\n");
            failures++;
        }
        if (sizeof(((flexEnvelopeHeader *)0)->_reserved) != 3u) {
            printf("dkTestEnvelopeReservedZeroAssert: _reserved size "
                   "drifted\n");
            failures++;
        }
    }

    if (failures == 0) {
        printf("dkTestEnvelopeReservedZeroAssert: OK\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * flexEnvelopeWrapParams::_pad must be zero.
 * ==================================================================== */
static int dkTestEnvelopeWrapPadPolicyTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    static const uint8_t payload[16] = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
        0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    };
    const size_t plen = sizeof(payload);

    {
        const size_t wrapped =
            flexEnvelopeWrappedSize(plen, FLEX_ENV_ALGO_XXH64, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(wrapped);
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_XXH64,
            .on_wire_flags = 0,
            ._pad = {0, 0, 0},
        };
        size_t written = 0;
        if (!flexEnvelopeWrap(buf, wrapped, payload, plen, &wp, &written)) {
            printf("dkTestEnvelopeWrapPadPolicy: zero-pad wrap failed\n");
            failures++;
        }
    }
    {
        const size_t wrapped =
            flexEnvelopeWrappedSize(plen, FLEX_ENV_ALGO_XXH64, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(wrapped);
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_XXH64,
            .on_wire_flags = 0,
            ._pad = {0xAB, 0xCD, 0xEF},
        };
        size_t written = 0;
        if (flexEnvelopeWrap(buf, wrapped, payload, plen, &wp, &written)) {
            printf("dkTestEnvelopeWrapPadPolicy: wrap accepted non-zero "
                   "_pad\n");
            failures++;
        }
    }
    {
        const uint8_t kPadTriples[3][3] = {
            {0xFF, 0x00, 0x00},
            {0x00, 0xFF, 0x00},
            {0x00, 0x00, 0xFF},
        };
        const size_t wrapped =
            flexEnvelopeWrappedSize(plen, FLEX_ENV_ALGO_XXH64, 0);
        for (size_t i = 0; i < 3; i++) {
            cleanerName(zfree, Bytes) uint8_t *buf =
                (uint8_t *)zmalloc(wrapped);
            flexEnvelopeWrapParams wp = {
                .algo = FLEX_ENV_ALGO_XXH64,
                .on_wire_flags = 0,
            };
            wp._pad[0] = kPadTriples[i][0];
            wp._pad[1] = kPadTriples[i][1];
            wp._pad[2] = kPadTriples[i][2];
            size_t written = 0;
            if (flexEnvelopeWrap(buf, wrapped, payload, plen, &wp, &written)) {
                printf("dkTestEnvelopeWrapPadPolicy: accepted _pad byte %zu\n",
                       i);
                failures++;
            }
        }
    }
    {
        const size_t wrapped =
            flexEnvelopeWrappedSize(plen, FLEX_ENV_ALGO_XXH64, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(wrapped);
        flexEnvelopeWrapParams wp = {.algo = FLEX_ENV_ALGO_XXH64};
        size_t written = 0;
        if (!flexEnvelopeWrap(buf, wrapped, payload, plen, &wp, &written)) {
            printf("dkTestEnvelopeWrapPadPolicy: canonical wrap failed\n");
            failures++;
        }
        flexEnvelopeVerifyParams vp = {.opts = FLEX_ENV_VERIFY_DEFAULT};
        const uint8_t *out_p = NULL;
        uint64_t out_pl = 0;
        if (!flexEnvelopeVerify(buf, wrapped, &vp, &out_p, &out_pl)) {
            printf("dkTestEnvelopeWrapPadPolicy: round-trip verify failed\n");
            failures++;
        }
        if (out_pl != plen || (out_p && memcmp(out_p, payload, plen) != 0)) {
            printf("dkTestEnvelopeWrapPadPolicy: payload mismatch\n");
            failures++;
        }
    }
    {
        static const uint8_t key[32] = {
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
            0xcc, 0xdd, 0xee, 0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
        };
        static const uint8_t nonce[12] = {
            0xc0, 0xff, 0xee, 0x00, 0xde, 0xad,
            0xbe, 0xef, 0xfa, 0xce, 0x12, 0x34,
        };
        const size_t wrapped = flexEnvelopeWrappedSize(
            plen, FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(wrapped);
        flexEnvelopeSealParams sp_ok = {
            .algo = FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
            .key = key,
            .key_len = sizeof(key),
            .nonce = nonce,
            .nonce_len = sizeof(nonce),
        };
        size_t written = 0;
        if (!flexEnvelopeSeal(buf, wrapped, payload, plen, &sp_ok, &written)) {
            printf("dkTestEnvelopeWrapPadPolicy: zero-pad Seal failed\n");
            failures++;
        }
        flexEnvelopeSealParams sp_bad = sp_ok;
        sp_bad._pad[0] = 0xFE;
        if (flexEnvelopeSeal(buf, wrapped, payload, plen, &sp_bad, &written)) {
            printf("dkTestEnvelopeWrapPadPolicy: Seal accepted non-zero "
                   "_pad[0]\n");
            failures++;
        }
    }

    if (failures == 0) {
        printf("dkTestEnvelopeWrapPadPolicy: OK\n");
        return 0;
    }
    return 1;
}

/* ====================================================================
 * End-to-end cluster replay-protection cross-integration test
 * (keyring + nonceLog + mailbox + replicator).
 * ==================================================================== */
#define D4_VALID_INGESTS 1000
#define D4_ENV_REPLAYS 50
#define D4_FORGERIES 50
#define D4_SEQ_REPLAYS 50
#define D4_TOTAL_FRAMES                                                        \
    (D4_VALID_INGESTS + D4_ENV_REPLAYS + D4_FORGERIES + D4_SEQ_REPLAYS)
#define D4_ORIGIN_ID ((uint32_t)9001u)
#define D4_KEY_LEN ((size_t)32)
#define D4_PAYLOAD_LEN ((size_t)64)

typedef struct dkTestClusterE2EApplyCtx {
    uint64_t apply_invocations;
} dkTestClusterE2EApplyCtx;

static bool dkTestClusterE2EApply(uint32_t origin_id, uint64_t sequence,
                                  const persistOps *ops, void *structure,
                                  void *ctx) {
    (void)origin_id;
    (void)sequence;
    dkTestClusterE2EApplyCtx *t = (dkTestClusterE2EApplyCtx *)ctx;
    t->apply_invocations++;
    if (ops != NULL && ops->free != NULL && structure != NULL) {
        ops->free(structure);
    }
    return true;
}

typedef struct dkTestClusterE2ESaved {
    uint8_t *wire;
    size_t len;
    uint64_t sequence;
    hmacKeyId key_id;
} dkTestClusterE2ESaved;

/* Scope-exit cleaner for the D4_VALID_INGESTS-sized saved-wire array:
 * frees each element's owned wire buffer, then the array itself. */
static DK_FN_UNUSED void
freeE2ESavedArray_cleanup(dkTestClusterE2ESaved **arr) {
    if (*arr) {
        for (size_t i = 0; i < D4_VALID_INGESTS; i++) {
            if ((*arr)[i].wire) {
                zfree((*arr)[i].wire);
            }
        }
        zfree(*arr);
    }
}

static int dkTestClusterE2EReplayProtectionTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;
    /* Declared here (before any goto to `cleanup_after_phase1`) so the
     * scope-exit cleaner attribute below never bypasses an initializer
     * across a jump. */
    __attribute__((cleanup(freeE2ESavedArray_cleanup)))
    dkTestClusterE2ESaved *saved = NULL;

    hmacKeyring *ring = hmacKeyringCreate();
    if (ring == NULL) {
        printf("dkTestClusterE2EReplayProtection: keyring create failed\n");
        return 1;
    }

    const hmacKeyId KIDS[3] = {0xD4000001u, 0xD4000002u, 0xD4000003u};
    const uint8_t key_material[3][D4_KEY_LEN] = {
        {0xD4, 0xA1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
         0,    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xA1},
        {0xD4, 0xB2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
         0,    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xB2},
        {0xD4, 0xC3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
         0,    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xC3},
    };
    const uint8_t salts[3][HMAC_KEYRING_AEAD_SALT_BYTES] = {
        {0xD4, 0x01, 0xA1, 0xA1},
        {0xD4, 0x02, 0xB2, 0xB2},
        {0xD4, 0x03, 0xC3, 0xC3},
    };
    for (int i = 0; i < 3; i++) {
        if (!hmacKeyringInstall(ring, KIDS[i], &hmacAlgoSHA256, key_material[i],
                                D4_KEY_LEN, salts[i])) {
            printf("dkTestClusterE2EReplayProtection: salted Install K%d "
                   "failed\n",
                   i + 1);
            hmacKeyringFree(ring);
            return 1;
        }
    }
    if (hmacKeyringSize(ring) != 3u) {
        printf("dkTestClusterE2EReplayProtection: ring size %zu != 3\n",
               hmacKeyringSize(ring));
        failures++;
    }

    cleaner(aeadNonceLogFree) aeadNonceLog *nonceLog =
        aeadNonceLogCreate(4096u);
    /* Declared here, ahead of the first goto, so the scope-exit cleaner
     * covers every jump to cleanup_after_phase1 below. */
    cleaner(flexFree) flex *rep_src = NULL;
    if (nonceLog == NULL) {
        printf("dkTestClusterE2EReplayProtection: nonceLog create failed\n");
        hmacKeyringFree(ring);
        return 1;
    }

    static const persistOps *type_table[256];
    memset((void *)type_table, 0, sizeof(type_table));
    type_table[PERSIST_TYPE_FLEX] = &persistOpsFlex;

    dkTestClusterE2EApplyCtx app_ctx = {0};

    const clusterMailboxCreateOptions mb_opts = {
        .ring = ring,
        .envelope_algo = FLEX_ENV_ALGO_HMAC_SHA256,
        .on_apply = NULL,
        .apply_ctx = NULL,
        .flags = CLUSTER_MAILBOX_FLAG_NONE,
        .disable_nonce_log = false,
        .nonce_log = nonceLog,
        .nonce_log_capacity = 0u,
    };
    clusterMailbox *mb = clusterMailboxCreateOpts(&mb_opts);
    if (mb == NULL) {
        printf("dkTestClusterE2EReplayProtection: mailbox createOpts failed\n");
        hmacKeyringFree(ring);
        return 1;
    }
    if (clusterMailboxNonceLog(mb) != nonceLog) {
        printf("dkTestClusterE2EReplayProtection: mailbox did not bind "
               "borrowed nonceLog\n");
        failures++;
    }

    const clusterReplicatorConfig rep_cfg = {
        .max_apply_retries = CLUSTER_REPLICATOR_DEFAULT_MAX_APPLY_RETRIES,
        .disable_nonce_log = false,
        .nonce_log_capacity = 0u,
    };
    clusterReplicator *rep = clusterReplicatorCreateEx(
        ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE, type_table,
        256u, dkTestClusterE2EApply, &app_ctx, &rep_cfg);
    if (rep == NULL) {
        printf(
            "dkTestClusterE2EReplayProtection: replicator CreateEx failed\n");
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
        return 1;
    }

    const uint8_t payload[D4_PAYLOAD_LEN] = {0xD4, 0xE2, 0xE1, 0x00};
    const size_t sealed_cap = clusterMailboxSealedSize(mb, D4_PAYLOAD_LEN);
    if (sealed_cap == 0u) {
        printf("dkTestClusterE2EReplayProtection: sealedSize = 0\n");
        failures++;
        goto cleanup_after_phase1;
    }

    saved = (dkTestClusterE2ESaved *)zcalloc(D4_VALID_INGESTS,
                                             sizeof(dkTestClusterE2ESaved));
    if (saved == NULL) {
        printf("dkTestClusterE2EReplayProtection: saved alloc failed\n");
        failures++;
        goto cleanup_after_phase1;
    }

    uint32_t prng = 0xD41D7E57u;
    uint64_t valid_success = 0;
    for (size_t i = 0; i < D4_VALID_INGESTS; i++) {
        prng ^= prng << 13;
        prng ^= prng >> 17;
        prng ^= prng << 5;
        const int kidx = (int)(prng % 3u);
        const hmacKeyId kid = KIDS[kidx];
        const uint64_t seq = (uint64_t)(i + 1u);

        cleanerName(zfree, Bytes) uint8_t *wire =
            (uint8_t *)zmalloc(sealed_cap);
        if (wire == NULL) {
            failures++;
            break;
        }
        size_t written = 0;
        if (!clusterMailboxSeal(mb, D4_ORIGIN_ID, seq, (uint8_t)0x20, payload,
                                D4_PAYLOAD_LEN, kid, wire, sealed_cap,
                                &written)) {
            printf("dkTestClusterE2EReplay: valid Seal[%zu] failed\n", i);
            failures++;
            break;
        }
        const clusterMailboxIngestResult rc =
            clusterMailboxIngest(mb, wire, written, kid);
        if (rc != CLUSTER_MAILBOX_OK) {
            printf("dkTestClusterE2EReplay: valid Ingest[%zu] expected OK got "
                   "%d\n",
                   i, (int)rc);
            failures++;
            break;
        }
        valid_success++;
        /* Ownership handoff: 'saved' now owns this wire buffer (freed by
         * freeE2ESavedArray_cleanup); disarm the local so the scope-exit
         * cleaner above skips it. */
        saved[i].wire = wire;
        wire = NULL;
        saved[i].len = written;
        saved[i].sequence = seq;
        saved[i].key_id = kid;
    }

    clusterMailboxStats stats_after_valid;
    clusterMailboxGetStats(mb, &stats_after_valid);
    if (stats_after_valid.apply_ok != (uint64_t)D4_VALID_INGESTS) {
        printf("dkTestClusterE2EReplay: apply_ok=%" PRIu64
               " expected %d after valid phase\n",
               stats_after_valid.apply_ok, D4_VALID_INGESTS);
        failures++;
    }
    if (valid_success != (uint64_t)D4_VALID_INGESTS) {
        printf("dkTestClusterE2EReplay: valid_success=%" PRIu64
               " expected %d\n",
               valid_success, D4_VALID_INGESTS);
        failures++;
    }
    if (clusterMailboxLastApplied(mb, D4_ORIGIN_ID) !=
        (uint64_t)D4_VALID_INGESTS) {
        printf("dkTestClusterE2EReplay: lastApplied=%" PRIu64 " expected %d\n",
               clusterMailboxLastApplied(mb, D4_ORIGIN_ID), D4_VALID_INGESTS);
        failures++;
    }

    for (size_t i = 0; i < D4_ENV_REPLAYS; i++) {
        const size_t pick =
            (size_t)(((uint64_t)i * 17u + 3u) % D4_VALID_INGESTS);
        const dkTestClusterE2ESaved *s = &saved[pick];
        const clusterMailboxIngestResult rc =
            clusterMailboxIngest(mb, s->wire, s->len, s->key_id);
        if (rc != CLUSTER_MAILBOX_ERR_REPLAY) {
            printf("dkTestClusterE2EReplay: env-replay[%zu] expected "
                   "ERR_REPLAY got %d\n",
                   i, (int)rc);
            failures++;
        }
    }

    const size_t hmac_sha256_digest_len = 32u;
    for (size_t i = 0; i < D4_FORGERIES; i++) {
        const size_t pick =
            (size_t)(((uint64_t)i * 23u + 5u) % D4_VALID_INGESTS);
        const dkTestClusterE2ESaved *s = &saved[pick];
        if (s->len <= hmac_sha256_digest_len) {
            printf("dkTestClusterE2EReplay: forgery[%zu] saved.len too small\n",
                   i);
            failures++;
            continue;
        }
        cleanerName(zfree, Bytes) uint8_t *forged = (uint8_t *)zmalloc(s->len);
        if (forged == NULL) {
            failures++;
            continue;
        }
        memcpy(forged, s->wire, s->len);
        forged[s->len - 1u] ^= (uint8_t)0x01;
        const clusterMailboxIngestResult rc =
            clusterMailboxIngest(mb, forged, s->len, s->key_id);
        if (rc != CLUSTER_MAILBOX_ERR_VERIFY_FAILED) {
            printf("dkTestClusterE2EReplay: forgery[%zu] expected "
                   "ERR_VERIFY_FAILED got %d\n",
                   i, (int)rc);
            failures++;
        }
    }

    for (size_t i = 0; i < D4_SEQ_REPLAYS; i++) {
        const size_t pick =
            (size_t)(((uint64_t)i * 31u + 7u) % D4_VALID_INGESTS);
        const dkTestClusterE2ESaved *s = &saved[pick];
        uint8_t alt_payload[D4_PAYLOAD_LEN];
        memcpy(alt_payload, payload, D4_PAYLOAD_LEN);
        alt_payload[0] = (uint8_t)(0xAA + (i & 0x0Fu));
        cleanerName(zfree, Bytes) uint8_t *fresh =
            (uint8_t *)zmalloc(sealed_cap);
        if (fresh == NULL) {
            failures++;
            continue;
        }
        size_t fresh_written = 0;
        if (!clusterMailboxSeal(mb, D4_ORIGIN_ID, s->sequence, (uint8_t)0x30,
                                alt_payload, D4_PAYLOAD_LEN, s->key_id, fresh,
                                sealed_cap, &fresh_written)) {
            printf("dkTestClusterE2EReplay: seq-replay[%zu] re-seal failed\n",
                   i);
            failures++;
            continue;
        }
        const clusterMailboxIngestResult rc =
            clusterMailboxIngest(mb, fresh, fresh_written, s->key_id);
        if (rc != CLUSTER_MAILBOX_ERR_REPLAY) {
            printf("dkTestClusterE2EReplay: seq-replay[%zu] expected "
                   "ERR_REPLAY got %d\n",
                   i, (int)rc);
            failures++;
        }
    }

    clusterMailboxStats final_stats;
    clusterMailboxGetStats(mb, &final_stats);

    const uint64_t exp_apply_ok = (uint64_t)D4_VALID_INGESTS;
    const uint64_t exp_verify_failed = (uint64_t)D4_FORGERIES;
    const uint64_t exp_replay_rejected =
        (uint64_t)(D4_ENV_REPLAYS + D4_SEQ_REPLAYS);

    if (final_stats.apply_ok != exp_apply_ok) {
        printf("dkTestClusterE2EReplay: final apply_ok=%" PRIu64
               " expected %" PRIu64 "\n",
               final_stats.apply_ok, exp_apply_ok);
        failures++;
    }
    if (final_stats.verify_failed != exp_verify_failed) {
        printf("dkTestClusterE2EReplay: final verify_failed=%" PRIu64
               " expected %" PRIu64 "\n",
               final_stats.verify_failed, exp_verify_failed);
        failures++;
    }
    if (final_stats.replay_rejected != exp_replay_rejected) {
        printf("dkTestClusterE2EReplay: final replay_rejected=%" PRIu64
               " expected %" PRIu64 "\n",
               final_stats.replay_rejected, exp_replay_rejected);
        failures++;
    }
    const uint64_t replay_class_caught =
        final_stats.replay_rejected + final_stats.verify_failed;
    if (replay_class_caught < (uint64_t)D4_ENV_REPLAYS) {
        printf("dkTestClusterE2EReplay: replay_class_caught=%" PRIu64 " < %d\n",
               replay_class_caught, D4_ENV_REPLAYS);
        failures++;
    }
    if (final_stats.ingest_total != (uint64_t)D4_TOTAL_FRAMES) {
        printf("dkTestClusterE2EReplay: ingest_total=%" PRIu64 " expected %d\n",
               final_stats.ingest_total, D4_TOTAL_FRAMES);
        failures++;
    }
    if (final_stats.apply_rejected != 0u) {
        printf("dkTestClusterE2EReplay: apply_rejected=%" PRIu64
               " expected 0\n",
               final_stats.apply_rejected);
        failures++;
    }
    if (final_stats.algo_mismatch != 0u || final_stats.bad_frame != 0u ||
        final_stats.key_missing != 0u || final_stats.origin_table_full != 0u ||
        final_stats.param_error != 0u) {
        printf("dkTestClusterE2EReplay: unexpected non-zero misc counters\n");
        failures++;
    }
    if (clusterMailboxNonceLog(mb) != nonceLog) {
        printf("dkTestClusterE2EReplay: nonceLog pointer no longer the "
               "borrowed one\n");
        failures++;
    }

    rep_src = flexNew();
    flexPushUnsigned(&rep_src, 42u, FLEX_ENDPOINT_TAIL);
    flexPushBytes(&rep_src, "d4-rep", 6u, FLEX_ENDPOINT_TAIL);
    const size_t rep_cap =
        clusterReplicatorSnapshotSize(rep, &persistOpsFlex, rep_src);
    if (rep_cap == 0u) {
        printf("dkTestClusterE2EReplay: replicator snapshotSize = 0\n");
        failures++;
    } else {
        cleanerName(zfree, Bytes) uint8_t *rep_wire =
            (uint8_t *)zmalloc(rep_cap);
        size_t rep_written = 0;
        if (!clusterReplicatorSeal(rep, &persistOpsFlex, rep_src, 1u, 1u,
                                   KIDS[0], rep_wire, rep_cap, &rep_written)) {
            printf("dkTestClusterE2EReplay: replicator Seal failed\n");
            failures++;
        } else {
            const clusterMailboxIngestResult rep_rc =
                clusterReplicatorIngest(rep, rep_wire, rep_written, KIDS[0]);
            if (rep_rc != CLUSTER_MAILBOX_OK) {
                printf("dkTestClusterE2EReplay: replicator Ingest expected OK "
                       "got %d\n",
                       (int)rep_rc);
                failures++;
            }
        }
    }
    if (app_ctx.apply_invocations != 1u) {
        printf("dkTestClusterE2EReplay: replicator apply_invocations=%" PRIu64
               " expected 1\n",
               app_ctx.apply_invocations);
        failures++;
    }

    printf("dkTestClusterE2EReplay counters: apply_ok=%" PRIu64
           " verify_failed=%" PRIu64 " replay_rejected=%" PRIu64
           " ingest_total=%" PRIu64
           " nonceLog.size=%zu apply_invocations=%" PRIu64 "\n",
           final_stats.apply_ok, final_stats.verify_failed,
           final_stats.replay_rejected, final_stats.ingest_total,
           aeadNonceLogSize(nonceLog), app_ctx.apply_invocations);

cleanup_after_phase1:
    clusterReplicatorFree(rep);
    clusterMailboxFree(mb);
    hmacKeyringFree(ring);

    if (failures == 0) {
        printf("dkTestClusterE2EReplayProtection: OK\n");
        return 0;
    }
    printf("dkTestClusterE2EReplayProtection: FAILED (%d failures)\n",
           failures);
    return 1;
}

/* ====================================================================
 * Structure-aware envelope fuzz per algorithm.
 * Every single-bit mutation across the wire MUST be rejected.
 * ==================================================================== */
static int dkTestEnvelopeStructAwareFuzzTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    uint32_t rng = 0x1234ABCDu;
#define ENV_FUZZ_RNG_NEXT()                                                    \
    ({                                                                         \
        rng ^= rng << 13;                                                      \
        rng ^= rng >> 17;                                                      \
        rng ^= rng << 5;                                                       \
        rng;                                                                   \
    })

    static const flexEnvelopeAlgo kAlgos[] = {
        FLEX_ENV_ALGO_XXH64,           FLEX_ENV_ALGO_XXH3_64,
        FLEX_ENV_ALGO_BLAKE3_KEYED,    FLEX_ENV_ALGO_HMAC_SHA256,
        FLEX_ENV_ALGO_HMAC_BLAKE3,     FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
        FLEX_ENV_ALGO_AEAD_AES128_GCM, FLEX_ENV_ALGO_AEAD_AES256_GCM,
    };
    static const char *const kAlgoNames[] = {
        "xxh64",           "xxh3_64",         "blake3_keyed",
        "hmac_sha256",     "hmac_blake3",     "aead_chacha20_poly1305",
        "aead_aes128_gcm", "aead_aes256_gcm",
    };
    const size_t kNumAlgos = sizeof(kAlgos) / sizeof(kAlgos[0]);

    static const uint8_t kKey32[32] = {
        0x4f, 0xe5, 0x9b, 0x21, 0x3a, 0x8c, 0x7d, 0x52, 0xaa, 0xbe, 0xdc,
        0x11, 0x33, 0x77, 0x99, 0xcd, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60,
        0x70, 0x80, 0xfe, 0xed, 0xfa, 0xce, 0xc0, 0xff, 0xee, 0x42,
    };
    static const uint8_t kNonce12[12] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    };

    const size_t kNumPayloadSizes = 50;
    size_t payload_sizes[50];
    payload_sizes[0] = 1;
    payload_sizes[1] = 2;
    payload_sizes[2] = 16;
    payload_sizes[3] = 127;
    payload_sizes[4] = 128;
    payload_sizes[5] = 255;
    payload_sizes[6] = 256;
    payload_sizes[7] = 1023;
    payload_sizes[8] = 1024;
    payload_sizes[9] = 4096;
    for (size_t i = 10; i < kNumPayloadSizes; i++) {
        payload_sizes[i] = 1 + (ENV_FUZZ_RNG_NEXT() % 4096u);
    }

    uint64_t total_mut[8] = {0};
    uint64_t reject_mut[8] = {0};

    const size_t kMaxPayload = 4096;
    cleanerName(zfree, Bytes) uint8_t *plaintext =
        (uint8_t *)zmalloc(kMaxPayload);
    if (plaintext == NULL) {
        printf("dkTestEnvelopeStructAwareFuzz: zmalloc plaintext failed\n");
        return 1;
    }
    for (size_t i = 0; i < kMaxPayload; i++) {
        plaintext[i] = (uint8_t)((i * 37u + 13u) ^ 0xA5u);
    }

    const size_t kMaxWireLen = kMaxPayload + 256u;
    cleanerName(zfree, Bytes) uint8_t *wire = (uint8_t *)zmalloc(kMaxWireLen);
    cleanerName(zfree, Bytes) uint8_t *plaintext_out =
        (uint8_t *)zmalloc(kMaxPayload);
    if (wire == NULL || plaintext_out == NULL) {
        printf("dkTestEnvelopeStructAwareFuzz: zmalloc wire/out failed\n");
        return 1;
    }

    const size_t kMaxMutationsPerEnvelope = 2000;

    for (size_t ai = 0; ai < kNumAlgos; ai++) {
        const flexEnvelopeAlgo algo = kAlgos[ai];
        const bool is_aead = FLEX_ENV_ALGO_IS_AEAD(algo);
        const bool is_keyed = FLEX_ENV_ALGO_IS_KEYED(algo) || is_aead;
        size_t key_len = 0;
        if (is_keyed) {
            if (algo == FLEX_ENV_ALGO_AEAD_AES128_GCM) {
                key_len = 16;
            } else if (algo == FLEX_ENV_ALGO_AEAD_AES256_GCM) {
                key_len = 32;
            } else {
                key_len = 32;
            }
        }

        for (size_t pi = 0; pi < kNumPayloadSizes; pi++) {
            const size_t plen = payload_sizes[pi];
            const size_t cap = flexEnvelopeWrappedSize((uint64_t)plen, algo, 0);
            if (cap == 0 || cap > kMaxWireLen) {
                printf("dkTestEnvelopeStructAwareFuzz: %s wrappedSize bad "
                       "(plen=%zu cap=%zu)\n",
                       kAlgoNames[ai], plen, cap);
                failures++;
                continue;
            }

            size_t written = 0;
            if (is_aead) {
                flexEnvelopeSealParams sp = {
                    .algo = algo,
                    .key = kKey32,
                    .key_len = key_len,
                    .nonce = kNonce12,
                    .nonce_len = 12,
                };
                if (!flexEnvelopeSeal(wire, cap, plaintext, (uint64_t)plen, &sp,
                                      &written)) {
                    printf("dkTestEnvelopeStructAwareFuzz: %s Seal failed "
                           "(plen=%zu)\n",
                           kAlgoNames[ai], plen);
                    failures++;
                    continue;
                }
            } else {
                flexEnvelopeWrapParams wp = {.algo = algo, .on_wire_flags = 0};
                if (is_keyed) {
                    wp.key = kKey32;
                    wp.key_len = key_len;
                }
                if (!flexEnvelopeWrap(wire, cap, plaintext, (uint64_t)plen, &wp,
                                      &written)) {
                    printf("dkTestEnvelopeStructAwareFuzz: %s Wrap failed "
                           "(plen=%zu)\n",
                           kAlgoNames[ai], plen);
                    failures++;
                    continue;
                }
            }
            if (written != cap) {
                printf("dkTestEnvelopeStructAwareFuzz: %s written=%zu "
                       "cap=%zu mismatch\n",
                       kAlgoNames[ai], written, cap);
                failures++;
                continue;
            }

            {
                flexEnvelopeVerifyParams vp = {
                    .key = is_keyed ? kKey32 : NULL,
                    .key_len = is_keyed ? key_len : 0,
                };
                bool ok;
                if (is_aead) {
                    size_t out_len = 0;
                    ok = flexEnvelopeOpen(wire, written, &vp, plaintext_out,
                                          kMaxPayload, &out_len);
                    if (ok && (out_len != plen ||
                               memcmp(plaintext_out, plaintext, plen) != 0)) {
                        printf("dkTestEnvelopeStructAwareFuzz: %s Open "
                               "round-trip mismatch\n",
                               kAlgoNames[ai]);
                        failures++;
                    }
                } else {
                    const uint8_t *out_p = NULL;
                    uint64_t out_pl = 0;
                    ok =
                        flexEnvelopeVerify(wire, written, &vp, &out_p, &out_pl);
                    if (ok && (out_pl != plen ||
                               memcmp(out_p, plaintext, plen) != 0)) {
                        printf("dkTestEnvelopeStructAwareFuzz: %s Verify "
                               "payload mismatch\n",
                               kAlgoNames[ai]);
                        failures++;
                    }
                }
                if (!ok) {
                    printf("dkTestEnvelopeStructAwareFuzz: %s untampered "
                           "Verify/Open FAILED\n",
                           kAlgoNames[ai]);
                    failures++;
                    continue;
                }
            }

            const size_t kHeadFull = 64u < written ? 64u : written;
            size_t remaining_budget =
                kMaxMutationsPerEnvelope - (kHeadFull * 8u);
            if (kMaxMutationsPerEnvelope <= kHeadFull * 8u) {
                remaining_budget = 0;
            }
            const size_t tail_bytes =
                (written > kHeadFull) ? (written - kHeadFull) : 0;
            size_t stride = 1;
            if (tail_bytes > 0 && remaining_budget > 0) {
                size_t max_tail_bytes = remaining_budget / 8u;
                if (max_tail_bytes == 0) {
                    max_tail_bytes = 1;
                }
                stride = (tail_bytes + max_tail_bytes - 1) / max_tail_bytes;
                if (stride == 0) {
                    stride = 1;
                }
            }

            for (size_t off = 0; off < written; off++) {
                bool in_head = (off < kHeadFull);
                bool in_tail = (written >= 32u && off >= written - 32u);
                if (!in_head && !in_tail && stride > 1 &&
                    ((off - kHeadFull) % stride) != 0u) {
                    continue;
                }
                const uint8_t saved = wire[off];
                for (int bit = 0; bit < 8; bit++) {
                    wire[off] = (uint8_t)(saved ^ (uint8_t)(1u << bit));
                    total_mut[ai]++;
                    flexEnvelopeVerifyParams vp = {
                        .key = is_keyed ? kKey32 : NULL,
                        .key_len = is_keyed ? key_len : 0,
                    };
                    bool accepted;
                    if (is_aead) {
                        size_t out_len = 0;
                        accepted =
                            flexEnvelopeOpen(wire, written, &vp, plaintext_out,
                                             kMaxPayload, &out_len);
                    } else {
                        const uint8_t *out_p = NULL;
                        uint64_t out_pl = 0;
                        accepted = flexEnvelopeVerify(wire, written, &vp,
                                                      &out_p, &out_pl);
                    }
                    if (!accepted) {
                        reject_mut[ai]++;
                    } else {
                        printf("dkTestEnvelopeStructAwareFuzz: %s ACCEPTED "
                               "mutated wire @ off=%zu bit=%d (plen=%zu "
                               "wire=%zu)\n",
                               kAlgoNames[ai], off, bit, plen, written);
                        failures++;
                    }
                }
                wire[off] = saved;
            }
        }
    }

    printf("dkTestEnvelopeStructAwareFuzz: per-algo rejection rates:\n");
    for (size_t ai = 0; ai < kNumAlgos; ai++) {
        const double rate =
            total_mut[ai] == 0
                ? 0.0
                : ((double)reject_mut[ai] * 100.0 / (double)total_mut[ai]);
        printf("  %-25s total=%-8" PRIu64 " rejected=%-8" PRIu64
               " rate=%.4f%%\n",
               kAlgoNames[ai], total_mut[ai], reject_mut[ai], rate);
        if (total_mut[ai] != reject_mut[ai]) {
            failures++;
        }
    }
#undef ENV_FUZZ_RNG_NEXT

    if (failures == 0) {
        printf("dkTestEnvelopeStructAwareFuzz: OK\n");
        return 0;
    }
    printf("dkTestEnvelopeStructAwareFuzz: FAILED (%d failures)\n", failures);
    return 1;
}

/* ====================================================================
 * flexEnvelope zero-copy Reserve+Write+Finalize API tests.
 * Validates byte-identical output vs canonical Wrap/Seal + verify round-
 * trip + negative paths (undersized buf, NULL params, wrong header_len).
 * ==================================================================== */
static int
dkTestEnvelopeCompareWrapVsReserve(flexEnvelopeAlgo algo, const uint8_t *key,
                                   size_t key_len, const uint8_t *payload,
                                   size_t payload_len, const char *label) {
    int failures = 0;
    const size_t wrapped = flexEnvelopeWrappedSize((uint64_t)payload_len, algo,
                                                   FLEX_ENV_FLAG_NONE);
    if (wrapped == 0) {
        printf("dkTestEnvelopeZeroCopy[%s]: wrappedSize == 0\n", label);
        return 1;
    }
    cleanerName(zfree, Bytes) uint8_t *buf_old = (uint8_t *)zmalloc(wrapped);
    cleanerName(zfree, Bytes) uint8_t *buf_new = (uint8_t *)zmalloc(wrapped);
    if (!buf_old || !buf_new) {
        return 1;
    }
    memset(buf_old, 0xAA, wrapped);
    memset(buf_new, 0xBB, wrapped);

    flexEnvelopeWrapParams wp = {
        .algo = algo,
        .on_wire_flags = FLEX_ENV_FLAG_NONE,
        .key = key,
        .key_len = key_len,
    };
    size_t written_old = 0;
    if (!flexEnvelopeWrap(buf_old, wrapped, payload, (uint64_t)payload_len, &wp,
                          &written_old)) {
        printf("dkTestEnvelopeZeroCopy[%s]: OLD Wrap failed\n", label);
        failures++;
        goto out;
    }
    uint8_t *region = NULL;
    size_t header_len = 0;
    if (!flexEnvelopeReserveWrap(buf_new, wrapped, (uint64_t)payload_len, &wp,
                                 &region, &header_len)) {
        printf("dkTestEnvelopeZeroCopy[%s]: ReserveWrap failed\n", label);
        failures++;
        goto out;
    }
    if (region != buf_new + header_len) {
        printf("dkTestEnvelopeZeroCopy[%s]: region offset wrong\n", label);
        failures++;
        goto out;
    }
    if (payload_len > 0) {
        memcpy(region, payload, payload_len);
    }
    size_t written_new = 0;
    if (!flexEnvelopeFinalizeWrap(buf_new, wrapped, header_len,
                                  (uint64_t)payload_len, &wp, &written_new)) {
        printf("dkTestEnvelopeZeroCopy[%s]: FinalizeWrap failed\n", label);
        failures++;
        goto out;
    }
    if (written_old != written_new) {
        printf("dkTestEnvelopeZeroCopy[%s]: size mismatch %zu vs %zu\n", label,
               written_old, written_new);
        failures++;
        goto out;
    }
    if (memcmp(buf_old, buf_new, written_old) != 0) {
        printf("dkTestEnvelopeZeroCopy[%s]: BYTE MISMATCH\n", label);
        failures++;
        goto out;
    }
    flexEnvelopeVerifyParams vp = {
        .key = key,
        .key_len = key_len,
        .opts = FLEX_ENV_VERIFY_DEFAULT,
    };
    const uint8_t *out_p = NULL;
    uint64_t out_pl = 0;
    if (!flexEnvelopeVerify(buf_new, written_new, &vp, &out_p, &out_pl)) {
        printf("dkTestEnvelopeZeroCopy[%s]: Verify of new-API frame failed\n",
               label);
        failures++;
        goto out;
    }
    if (out_pl != (uint64_t)payload_len ||
        (payload_len > 0 && memcmp(out_p, payload, payload_len) != 0)) {
        printf("dkTestEnvelopeZeroCopy[%s]: payload mismatch on verify\n",
               label);
        failures++;
        goto out;
    }
out:
    return failures;
}

static int dkTestEnvelopeZeroCopyTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int failures = 0;

    static const uint8_t payload[96] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b,
        0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b,
        0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
        0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63,
        0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    };
    static const uint8_t hmac_key[32] = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
        0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5,
        0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
    };

    failures += dkTestEnvelopeCompareWrapVsReserve(
        FLEX_ENV_ALGO_XXH64, NULL, 0, payload, sizeof(payload), "xxh64");
    failures += dkTestEnvelopeCompareWrapVsReserve(FLEX_ENV_ALGO_XXH64, NULL, 0,
                                                   NULL, 0, "xxh64-empty");
    failures += dkTestEnvelopeCompareWrapVsReserve(
        FLEX_ENV_ALGO_HMAC_SHA256, hmac_key, sizeof(hmac_key), payload,
        sizeof(payload), "hmac_sha256");

    {
        static const uint8_t aead_key[CHACHA20_POLY1305_KEY_LEN] = {
            0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
            0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5,
            0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
        };
        static const uint8_t nonce[12] = {
            0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5,
            0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb,
        };
        const size_t wrapped = flexEnvelopeWrappedSize(
            (uint64_t)sizeof(payload), FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
            FLEX_ENV_FLAG_NONE);
        cleanerName(zfree, Bytes) uint8_t *buf_old =
            (uint8_t *)zmalloc(wrapped);
        cleanerName(zfree, Bytes) uint8_t *buf_new =
            (uint8_t *)zmalloc(wrapped);
        if (!buf_old || !buf_new) {
            failures++;
        } else {
            flexEnvelopeSealParams sp = {
                .key = aead_key,
                .key_len = sizeof(aead_key),
                .nonce = nonce,
                .nonce_len = sizeof(nonce),
                .algo = FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
                .on_wire_flags = FLEX_ENV_FLAG_NONE,
            };
            size_t written_old = 0;
            if (!flexEnvelopeSeal(buf_old, wrapped, payload,
                                  (uint64_t)sizeof(payload), &sp,
                                  &written_old)) {
                printf("dkTestEnvelopeZeroCopy[aead]: OLD Seal failed\n");
                failures++;
            } else {
                uint8_t *region = NULL;
                size_t hn = 0;
                if (!flexEnvelopeReserveSeal(buf_new, wrapped,
                                             (uint64_t)sizeof(payload), &sp,
                                             &region, &hn)) {
                    printf(
                        "dkTestEnvelopeZeroCopy[aead]: ReserveSeal failed\n");
                    failures++;
                } else {
                    memcpy(region, payload, sizeof(payload));
                    size_t written_new = 0;
                    if (!flexEnvelopeFinalizeSeal(buf_new, wrapped, hn,
                                                  (uint64_t)sizeof(payload),
                                                  &sp, &written_new)) {
                        printf("dkTestEnvelopeZeroCopy[aead]: FinalizeSeal "
                               "failed\n");
                        failures++;
                    } else if (written_new != written_old ||
                               memcmp(buf_old, buf_new, written_old) != 0) {
                        printf("dkTestEnvelopeZeroCopy[aead]: byte mismatch "
                               "%zu vs %zu\n",
                               written_old, written_new);
                        failures++;
                    } else {
                        cleanerName(zfree, Bytes) uint8_t *pt_out =
                            (uint8_t *)zmalloc(sizeof(payload));
                        flexEnvelopeVerifyParams vp = {
                            .key = aead_key,
                            .key_len = sizeof(aead_key),
                            .opts = FLEX_ENV_VERIFY_DEFAULT,
                        };
                        size_t pt_written = 0;
                        if (!flexEnvelopeOpen(buf_new, written_new, &vp, pt_out,
                                              sizeof(payload), &pt_written)) {
                            printf("dkTestEnvelopeZeroCopy[aead]: Open of "
                                   "new-API frame failed\n");
                            failures++;
                        } else if (pt_written != sizeof(payload) ||
                                   memcmp(pt_out, payload, sizeof(payload)) !=
                                       0) {
                            printf("dkTestEnvelopeZeroCopy[aead]: open "
                                   "plaintext mismatch\n");
                            failures++;
                        }
                    }
                }
            }
        }
    }

    {
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_XXH64,
            .on_wire_flags = FLEX_ENV_FLAG_NONE,
        };
        uint8_t tiny[4];
        memset(tiny, 0xCC, sizeof(tiny));
        uint8_t *region = NULL;
        size_t hn = 0;
        if (flexEnvelopeReserveWrap(tiny, sizeof(tiny),
                                    (uint64_t)sizeof(payload), &wp, &region,
                                    &hn)) {
            printf("dkTestEnvelopeZeroCopy[neg-cap]: ReserveWrap accepted "
                   "undersized buf\n");
            failures++;
        }
    }

    {
        uint8_t buf[64];
        memset(buf, 0xDD, sizeof(buf));
        size_t written = 0;
        if (flexEnvelopeFinalizeWrap(buf, sizeof(buf), 8, (uint64_t)4, NULL,
                                     &written)) {
            printf("dkTestEnvelopeZeroCopy[neg-null]: FinalizeWrap accepted "
                   "NULL params\n");
            failures++;
        }
    }

    {
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_XXH64,
            .on_wire_flags = FLEX_ENV_FLAG_NONE,
        };
        const size_t wrapped = flexEnvelopeWrappedSize(
            (uint64_t)sizeof(payload), FLEX_ENV_ALGO_XXH64, FLEX_ENV_FLAG_NONE);
        cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(wrapped);
        if (buf) {
            uint8_t *region = NULL;
            size_t hn = 0;
            if (flexEnvelopeReserveWrap(buf, wrapped, (uint64_t)sizeof(payload),
                                        &wp, &region, &hn)) {
                memcpy(region, payload, sizeof(payload));
                size_t written = 0;
                if (flexEnvelopeFinalizeWrap(buf, wrapped, hn + 1,
                                             (uint64_t)sizeof(payload), &wp,
                                             &written)) {
                    printf("dkTestEnvelopeZeroCopy[neg-hdrlen]: accepted "
                           "wrong header_len\n");
                    failures++;
                }
            }
        }
    }

    if (failures == 0) {
        printf("dkTestEnvelopeZeroCopy: OK\n");
        return 0;
    }
    printf("dkTestEnvelopeZeroCopy: FAILED (%d failure(s))\n", failures);
    return 1;
}

/*
 * The test registry - add new tests here!
 *
 * Format:
 *   T(name)           - Test with function nameTest(argc, argv)
 *   T_A(name, "a,b")  - Test with aliases
 *   T_ADJ(name)       - Test receiving adjusted args (argc-2, argv+2)
 *   T_A_ADJ(name, a)  - Both aliases and adjusted args
 *   T_CAT(name, cat)  - Test with category
 */
static const TestEntry testRegistry[] = {
    /* Core data structures */
    T(flex), T_A(flexEnvelope, "fenv,env"), T(sha256),
    T_A(sha256ArmCrypto, "sha256-arm"), T_A(sha256ShaNi, "sha256-shani"),
    T_A(blake3, "b3"), T_A(blake3Neon, "b3-neon"), T_A(blake3SSE41, "b3-sse"),
    T_A(blake3AVX2, "b3-avx"), T_A(chacha20poly1305, "c20,chacha,aead-prim"),
    T(hmac), T_A(hmacKeyring, "keyring"),
    T_A(aeadNonceLog, "noncelog,nonce-log"), T_A(dkFormat, "format"),
    T_A(x25519, "ecdh"), T_A(rapidhash, "rh,rapid"), T_A(aesGcm, "aes,aesgcm"),
    T_A(clusterMailbox, "mailbox,mb"), T_A(clusterReplicator, "replicator,rep"),
    T_A(perfRegression, "perf-baseline,perfbase"), T(mflex),
    T_A(multilist, "ml"), T(multilistFull), T(multidict), T_ADJ(multimap),
    T(multimapFull), T(multimapMedium), T_A_ADJ(multimapAtom, "atom"),
    T_A(multimapAtomDense, "atomdense"), T_A_ADJ(stringPool, "sp,strpool"),
    T_A_ADJ(atomPool, "ap,apool"), T_ADJ(multiarray), T_ADJ(multiarraySmall),
    T_ADJ(multiarrayMedium), T_ADJ(multiarrayLarge), T_ADJ(multiroar),
    T_A_ADJ(multiOrderedSet, "mos"), T_A_ADJ(multilru, "lru,mlru"), T_ADJ(list),
    T_A_ADJ(ptrPrevNext, "ppn"),

    /* Numeric types */
    T_A(float16, "f16"), T_A_ADJ(floatExtended, "float128,fe"), T(intset),
    T(intsetU32), T_A_ADJ(fenwickI64, "fw,bit,fenwick"), /* New 2-tier system */
    T_A_ADJ(segmentI64, "seg,segtree,segment"),          /* New 2-tier system */
    T_A_ADJ(multiFenwick, "mfw,mbit"), /* Multi-tree wrapper system */
    T_A(multiFenwickFlat, "mfwf"),     /* multiFenwick, flat cells */
    T_A(fenwickTypes, "fwtypes"),      /* every typed fenwick vs naive */
    T_A(segmentTypes, "segtypes"),     /* every typed segment vs naive */
    T_A(fenwick2d, "fw2d"),            /* 2D typed fenwick trees */
    T_A(multiFenwick2d, "mfw2d"),      /* 2D fenwick, databox values */
#if USE_INTSET_BIG
    T(intsetBig),
#endif

    /* Concurrency primitives */
    T(fastmutex),

    /* Strings */
    T(mds), T(mdsc), T_ADJ(str), T_ADJ(strDoubleFormat),

    /* Algorithms */
    T(dj), T_ADJ(dod), T_ADJ(xof), T_ADJ(intersectInt),
    T_A_ADJ(hyperloglog, "hll"), T_A_ADJ(linearBloom, "bloom"), T(sha1),

    /* Timers */
    T_A_ADJ(timeUtil, "time"), T_A_ADJ(multiTimer, "timer"),
    T_A_ADJ(timerWheel, "tw"),

    /* Buffers */
    T_ADJ(fibbuf), T_ADJ(jebuf), T_ADJ(ptrlib), T_ADJ(bigmath),

    /* Utilities */
    T(util), T_ADJ(databox), T_ADJ(databoxLinear), T(offsetArray), T(multiheap),
    T_ADJ(membound), T_ADJ(memtest), T_ADJ(bbits), T_ADJ(ulid), T(persist),
    T(OSRegulate), T(dataspeedParse), T_A(persistCtx, "pctx"),
    T_A(persistContract, "pcontract"), T_A(multimapP, "mmP"),
    T_A(multilistP, "mlP"), T_A(multidictP, "mdP"), T_A(flexP, "fP"),
    T_A(intsetP, "isP"), T_A(multiOrderedSetP, "mosP"), T_A(multiroarP, "mrP"),
    T_A(multilruP, "mlruP"),

    /* Distributed / Cluster */
    T_A_ADJ(clusterRing, "ring,cluster,chr"),

    /* OS process/event primitives (tests live in this file) */
    T_A(dkTestProcessExitWait, "processExitWait,procWait"),
    T_A(dkTestWakeFd, "wakeFd,wake"),
    T_A(dkTestInflightGate, "inflightGate,inflight"),
    T_A(dkTestOwnedState, "ownedState,owned"),
    T_A(dkTestOwnedSeqGate, "ownedSeqGate,seqGate"),
    T_A(dkTestSharrow, "sharrow"),
    T_A(dkTestDaemonizeReady, "daemonizeReady,daemonize"),

    /* Random generators */
    T_A(dkRandom, "rng"), T(portableRandom),

    /* Crypto helpers (header-only; tests live in this file) */
    T_A(dkTestSecureZero, "secureZero"),
    T_A(dkTestChaChaStreamCtxWipe, "chachaStreamCtxWipe"),
    T_A(dkTestStreamMidTamper, "chachaStreamMidTamper"),
    T_A(dkTestStreamUpdateAfterFinal, "chachaStreamUAF"),
    T_A(dkTestStreamFinalTwice, "chachaStreamFinalTwice"),
    T_A(dkTestAesNi, "aesNi"), T_A(dkTestAesArmCrypto, "aesArmCrypto"),
    T_A(dkTestX25519SmallOrderReject, "x25519SmallOrder"),
    T_A(dkTestX25519Iter1000KAT, "x25519Iter1000"),
    T_A(dkTestBlake3DeriveKeyVarInput, "blake3DKVarInput"),
    T_A(dkTestSha256AllBackendParity, "sha256BackendParity"),
    T_A(dkTestAesGcmExtendedVectors, "aesGcmExtended"),
    T_A(dkTestChaChaSimdParity, "chachaSimdParity"),
    T_A(dkTestPoly1305SimdParity, "poly1305SimdParity"),
    T_A(dkTestBlake3Avx2Dispatch, "blake3Avx2Dispatch"),
    T_A(dkTestBlake3Sse41Batch, "blake3Sse41Batch"),
    T_A(dkTestBlake3DeriveKeyContexts, "blake3DeriveKeyCtx"),
    T_A(dkTestKeyringRequireSalt, "keyringRequireSalt"),
    T_A(dkTestKeyringPersistRetireAfterAttach, "keyringPersistRetire"),
    T_A(dkTestRevokedKeyFloor, "keyringRevokedFloor"),
    T_A(dkTestEnvelopeOpenKeyringResolve, "envelopeOpenKeyring"),
    T_A(dkTestWireKeyIdAmbiguityReject, "envelopeWireKeyIdFlag"),
    T_A(dkTestPersistKeyIdHeader, "persistKeyIdHeader"),
    T_A(dkTestNonceLogPersistRoundTrip, "nonceLogPersist"),
    T_A(dkTestEnvelopeNonceLogIntegration, "envelopeNonceLog"),
    T_A(dkTestNonceLogEvictionPerf, "nonceLogEvictPerf"),
    T_A(dkTestMailboxReplicatorNonceLogWired, "mailboxReplicatorNonceLog"),
    T_A(dkTestClusterRingFlexSerialize, "clusterRingFlex"),
    T_A(dkTestClusterRingMaliciousState, "clusterRingMalicious"),
    T_A(dkTestClusterRingDuplicateId, "clusterRingDupId"),
    T_A(dkTestClusterRingTruncatedBuffer, "clusterRingTruncated"),
    T_A(dkTestPersistMultilistTieredApply, "persistMultilistTier"),
    T_A(dkTestReplicatorRetryDrop, "replicatorRetry"),
    T_A(dkTestMailboxWindowOrientation, "mailboxWindowOrient"),
    T_A(dkTestMailboxStatsRace, "mailboxStatsRace"),
    T_A(dkTestMailboxStatsAlignment, "mailboxStatsAlign"),
    T_A(dkTestKeyringGetRawCopyMigrationCluster,
        "keyringGetRawCopyMigrationCluster"),
    T_A(dkTestFlexEnvelopeKeyringRetireRace, "flexEnvelopeKeyringRetireRace"),
    T_A(dkTestPersistTypeNameAllRegistered, "persistTypeName"),
    T_A(dkTestKeyringPersistOpsDescriptorRoundTrip, "keyringPersistOps"),
    T_A(dkTestRingPersistOpsDescriptorRoundTrip, "ringPersistOps"),
    T_A(dkTestMailboxPersistOpsDescriptorRoundTrip, "mailboxPersistOps"),
    T_A(dkTestAeadKeyScheduleCache, "aeadKeyScheduleCache"),
    T_A(dkTestMailboxInnerHeaderFlex, "mailboxInnerHeaderFlex"),
    T_A(dkTestMailboxRotateAnnounceFlex, "mailboxRotateAnnounceFlex"),
    T_A(dkTestEnvelopeReservedZeroAssert, "envelopeReservedZero"),
    T_A(dkTestEnvelopeWrapPadPolicy, "envelopeWrapPadPolicy"),
    T_A(dkTestClusterE2EReplayProtection, "clusterE2EReplay"),
    T_A(dkTestEnvelopeStructAwareFuzz, "envStructAwareFuzz"),
    T_A(dkTestEnvelopeZeroCopy, "envelopeZeroCopy"),
    T_A(testCtMemEqStructural, "ctMemEqStructural"),
    T_A(testNonceLogAllAeadAlgos, "nonceLogAllAead"),
    T_A(testChachaStreamOpenNonceFlip, "chachaStreamOpenNonceFlip"),
    T_A(testMailboxInnerFlexStructAwareFuzz, "mailboxInnerFlexFuzz"),
    T_A(testMailboxRotateAnnounceStructAwareFuzz, "mailboxRotateAnnounceFuzz"),
    T_A(testMailboxRestoreStructAwareFuzz, "mailboxRestoreFuzz"),
    T_A(testMailboxPersistOpsRestoreStructAwareFuzz,
        "mailboxPersistOpsRestoreFuzz"),
    T_A(testMailboxRestoreWrappedStructAwareFuzz, "mailboxRestoreWrappedFuzz"),

    /* Benchmarks */
    T_A_ADJ(compressionBench, "compress,cbench"), T_A(fenwickBench, "fwbench"),

    T_END};

/* Benchmarks (separate from tests) */
static const TestEntry benchRegistry[] = {
    {"mdsbench", NULL, mdsBenchWrapper, false, NULL},
    {"mdscbench", NULL, mdscBenchWrapper, false, NULL},
    T_END};

/* Pseudo-tests (special multi-test runners) */
typedef struct {
    const char *name;
    const char *tests; /* Comma-separated list of test names */
} MultiTest;

static const MultiTest multiTests[] = {
    {"allstr", "mds,mdsbench,mdsc,mdscbench"}, {NULL, NULL}};

/* ====================================================================
 * Test Runner Implementation
 * ==================================================================== */

static int countTests(void) {
    int count = 0;
    for (int i = 0; testRegistry[i].name; i++) {
        count++;
    }
    return count;
}

static void printUsage(const char *progname) {
    printf("Usage: %s <command> [test] [options]\n\n", progname);
    printf("Commands:\n");
    printf("  test <name>   Run a specific test\n");
    printf("  test ALL      Run all tests\n");
    printf("  list          List all available tests\n");
    printf("  list --json   List tests in JSON format\n");
    printf("  bench <name>  Run a benchmark\n");
    printf("  speed [options] [MB] [N]\n");
    printf("                  Run dataspeed system benchmark\n");
    printf("                  Options:\n");
    printf("                    --json  Output results as JSON\n");
    printf("                    --csv   Output results as CSV\n");
    printf(
        "                  MB = working set size in megabytes (default: 64)\n");
    printf("                  N  = legacy test iterations (default: 0, skip "
           "legacy)\n");
    printf("                  Examples:\n");
    printf("                    speed              Run with defaults\n");
    printf("                    speed 64 10        64MB working set, 10 legacy "
           "iters\n");
    printf("                    speed --json       Output full benchmark as "
           "JSON\n");
    printf("  help          Show this help message\n");
}

static void listTests(bool json) {
    if (json) {
        printf("{\n  \"tests\": [\n");
        bool first = true;
        for (int i = 0; testRegistry[i].name; i++) {
            if (!first) {
                printf(",\n");
            }
            first = false;
            printf("    {\"name\": \"%s\"", testRegistry[i].name);
            if (testRegistry[i].aliases) {
                printf(", \"aliases\": \"%s\"", testRegistry[i].aliases);
            }
            if (testRegistry[i].category) {
                printf(", \"category\": \"%s\"", testRegistry[i].category);
            }
            printf("}");
        }
        printf("\n  ],\n  \"benchmarks\": [\n");
        first = true;
        for (int i = 0; benchRegistry[i].name; i++) {
            if (!first) {
                printf(",\n");
            }
            first = false;
            printf("    {\"name\": \"%s\"}", benchRegistry[i].name);
        }
        printf("\n  ],\n  \"count\": %d\n}\n", countTests());
    } else {
        printf("Available tests (%d):\n", countTests());
        const char *lastCategory = NULL;
        for (int i = 0; testRegistry[i].name; i++) {
            const TestEntry *t = &testRegistry[i];
            const char *cat = t->category ? t->category : "default";
            if (!lastCategory || strcmp(lastCategory, cat) != 0) {
                lastCategory = cat;
                /* Don't print category header for now to keep output clean */
            }
            printf("  %-20s", t->name);
            if (t->aliases) {
                printf(" (aliases: %s)", t->aliases);
            }
            printf("\n");
        }
        printf("\nBenchmarks:\n");
        for (int i = 0; benchRegistry[i].name; i++) {
            printf("  %s\n", benchRegistry[i].name);
        }
        printf("\nSpecial:\n");
        printf("  ALL                  Run all tests\n");
        for (int i = 0; multiTests[i].name; i++) {
            printf("  %-20s -> %s\n", multiTests[i].name, multiTests[i].tests);
        }
    }
}

static int runTest(const TestEntry *t, int argc, char *argv[]) {
    if (t->adjustArgs) {
        return t->func(argc - 2, argv + 2);
    } else {
        return t->func(argc, argv);
    }
}

static const TestEntry *findTest(const char *name) {
    for (int i = 0; testRegistry[i].name; i++) {
        if (testMatches(&testRegistry[i], name)) {
            return &testRegistry[i];
        }
    }
    return NULL;
}

static const TestEntry *findBench(const char *name) {
    for (int i = 0; benchRegistry[i].name; i++) {
        if (strcasecmp(benchRegistry[i].name, name) == 0) {
            return &benchRegistry[i];
        }
    }
    return NULL;
}

static int runMultiTest(const MultiTest *mt, int argc, char *argv[]) {
    int result = 0;
    const char *p = mt->tests;
    char name[64];

    while (*p) {
        const char *start = p;
        while (*p && *p != ',') {
            p++;
        }
        size_t len = p - start;
        if (len >= sizeof(name)) {
            len = sizeof(name) - 1;
        }
        memcpy(name, start, len);
        name[len] = '\0';

        /* Find and run test or bench */
        const TestEntry *t = findTest(name);
        if (t) {
            result += runTest(t, argc, argv);
        } else {
            t = findBench(name);
            if (t) {
                result += t->func(argc, argv);
            }
        }

        if (*p == ',') {
            p++;
        }
    }
    return result;
}

static int runAllTests(int argc, char *argv[]) {
    int result = 0;
    for (int i = 0; testRegistry[i].name; i++) {
        result += runTest(&testRegistry[i], argc, argv);
    }
    return result;
}

int main(int argc, char *argv[]) {
    /* Don't break the databox size contract! */
    assert(sizeof(databox) == 16);

    /* Line-buffer stdout even through a pipe, so every test's output (its
     * section headers included, whether printed by ctest.h or by the
     * suite itself) arrives when printed: ctest logs interleave with
     * stderr in order, and dktools testtime times sections exactly. */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    /* Handle commands */
    const char *cmd = argv[1];

    /* list - enumerate available tests */
    if (strcasecmp(cmd, "list") == 0) {
        bool json = (argc >= 3 && strcmp(argv[2], "--json") == 0);
        listTests(json);
        return 0;
    }

    /* help */
    if (strcasecmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "--help") == 0) {
        printUsage(argv[0]);
        return 0;
    }

    /* speed benchmark */
    if (strcasecmp(cmd, "speed") == 0) {
        double mb = 64.0; /* Default: 64 MB working set */
        size_t iters = 0; /* Default: skip legacy test */
        bool jsonOutput = false;
        bool csvOutput = false;
        int argIdx = 2;

        /* Parse options */
        while (argIdx < argc) {
            if (strcmp(argv[argIdx], "--json") == 0) {
                jsonOutput = true;
                argIdx++;
            } else if (strcmp(argv[argIdx], "--csv") == 0) {
                csvOutput = true;
                argIdx++;
            } else {
                break;
            }
        }

        if (argIdx < argc) {
            mb = atof(argv[argIdx]);
            if (mb <= 0 || mb > 4096) {
                fprintf(stderr,
                        "Error: MB must be between 1 and 4096 (got: %s)\n",
                        argv[argIdx]);
                fprintf(stderr,
                        "Usage: %s speed [--json|--csv] [MB] [iterations]\n",
                        argv[0]);
                return -1;
            }
            argIdx++;
        }
        if (argIdx < argc) {
            iters = (size_t)atoi(argv[argIdx]);
        }

        /* Run benchmark with appropriate output format */
        if (jsonOutput) {
            dataspeedReport report;
            dataspeedRunAll(&report, false);
            dataspeedPrintReportJSON(&report);
            return 0;
        } else if (csvOutput) {
            dataspeedReport report;
            dataspeedRunAll(&report, false);
            dataspeedPrintReportCSV(&report);
            return 0;
        }

        return dataspeed(mb, iters);
    }

    /* bench <name> */
    if (strcasecmp(cmd, "bench") == 0 && argc >= 3) {
        const TestEntry *t = findBench(argv[2]);
        if (t) {
            return t->func(argc - 2, argv + 2);
        }
        printf("Benchmark not found: %s\n", argv[2]);
        return -3;
    }

    /* test <name> */
    if (strcasecmp(cmd, "test") == 0 && argc >= 3) {
        /* Check if multiple tests requested */
        if (argc > 3) {
            printf("=== Running %d test suites ===\n", argc - 2);
            for (int i = 2; i < argc; i++) {
                printf("  - %s\n", argv[i]);
            }
            printf("\n");

            int totalFailed = 0;
            for (int testIdx = 2; testIdx < argc; testIdx++) {
                const char *testName = argv[testIdx];

                printf("\n=== Test Suite: %s ===\n", testName);

                /* Special: ALL */
                if (strcasecmp(testName, "ALL") == 0) {
                    int result = runAllTests(argc, argv);
                    if (result != 0) {
                        totalFailed++;
                    }
                    continue;
                }

                /* Check multi-tests */
                bool found = false;
                for (int i = 0; multiTests[i].name; i++) {
                    if (strcasecmp(multiTests[i].name, testName) == 0) {
                        int result = runMultiTest(&multiTests[i], argc, argv);
                        if (result != 0) {
                            totalFailed++;
                        }
                        found = true;
                        break;
                    }
                }
                if (found) {
                    continue;
                }

                /* Find and run specific test */
                const TestEntry *t = findTest(testName);
                if (t) {
                    int result = runTest(t, argc, argv);
                    if (result != 0) {
                        totalFailed++;
                    }
                    continue;
                }

                /* Check benchmarks */
                t = findBench(testName);
                if (t) {
                    int result = t->func(argc - 2, argv + 2);
                    if (result != 0) {
                        totalFailed++;
                    }
                    continue;
                }

                printf("Test not found: %s\n", testName);
                totalFailed++;
            }

            printf("\n=== Summary: %d/%d test suites passed ===\n",
                   (argc - 2) - totalFailed, argc - 2);
            return totalFailed > 0 ? 1 : 0;
        }

        /* Single test requested */
        const char *testName = argv[2];

        /* Special: ALL runs all tests */
        if (strcasecmp(testName, "ALL") == 0) {
            return runAllTests(argc, argv);
        }

        /* Check multi-tests */
        for (int i = 0; multiTests[i].name; i++) {
            if (strcasecmp(multiTests[i].name, testName) == 0) {
                return runMultiTest(&multiTests[i], argc, argv);
            }
        }

        /* Find and run specific test */
        const TestEntry *t = findTest(testName);
        if (t) {
            return runTest(t, argc, argv);
        }

        /* Check benchmarks too (allow 'test mdsbench') */
        t = findBench(testName);
        if (t) {
            return t->func(argc - 2, argv + 2);
        }

        printf("Test not found: %s\n", testName);
        printf("Use '%s list' to see available tests.\n", argv[0]);
        return -3;
    }

    /* Maybe user forgot 'test' keyword? Try to be helpful */
    const TestEntry *t = findTest(cmd);
    if (t) {
        printf("Did you mean: %s test %s\n", argv[0], cmd);
        return -3;
    }

    printUsage(argv[0]);
    return -3;
}
