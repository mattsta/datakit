#include "dkRandom.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>

#include "datakit.h"
#include "portableRandom.h"

/* Words generated per refill.  Each drawing thread carries this block
 * (512 bytes) beside its portableRandomState.  A refill is one
 * portableRandomFill64 call, so the pid check (a syscall on libcs that
 * do not cache the pid) and the refuel budget charge run once per block
 * rather than once per word, and a draw is a TLS read. */
#define DK_RANDOM_BLOCK_WORDS 64

/* Count of dkRandomBounded(0) calls; relaxed, nothing orders against
 * it. */
static _Atomic uint64_t zeroBoundTotal;

/* Fork generation.  A pthread_atfork child handler increments it, and
 * a thread whose block was filled under an older generation discards
 * the block before its next draw.  portableRandom's fork check runs
 * only when it generates, so without this a child of fork() would
 * return the rest of the parent's buffered block.  Relaxed is enough:
 * the handler runs on the forking thread, which is the only thread in
 * the child, before fork() returns there. */
static _Atomic uint64_t forkGeneration;
static pthread_once_t forkHandlerOnce = PTHREAD_ONCE_INIT;

static void dkRandomForkChild(void) {
    (void)atomic_fetch_add_explicit(&forkGeneration, 1, memory_order_relaxed);
}

static void dkRandomForkHandlerRegister(void) {
    (void)pthread_atfork(NULL, NULL, dkRandomForkChild);
}

/* Per-thread generator and output block, in one struct so a draw
 * resolves one TLS address (on Darwin each _Thread_local variable is
 * its own thunk call).  The fields a draw reads come first.  blockUsed
 * starts full so a thread's first draw refills. */
typedef struct dkRandomThread {
    uint32_t blockUsed;
    bool seeded;
    uint64_t blockGeneration;
    uint64_t block[DK_RANDOM_BLOCK_WORDS];
    portableRandomState prs;
} dkRandomThread;

static _Thread_local dkRandomThread dkRandomTls = {.blockUsed =
                                                       DK_RANDOM_BLOCK_WORDS};

/* Out of line so the draw path stays a load, a compare and a return. */
__attribute__((noinline)) static void
dkRandomRefill(dkRandomThread *const t, const uint64_t generation) {
    if (!t->seeded) {
        /* Seed with the real pid, never -1: portableRandom skips its
         * fork check for a state initialised with -1, and that check is
         * what re-keys a forked child.  portableRandom_init keys from
         * the OS and leaves the refuel budget at 0, so the first fill
         * also runs a full portableRandom_stir. */
        (void)pthread_once(&forkHandlerOnce, dkRandomForkHandlerRegister);
        portableRandom_init(&t->prs, getpid());
        t->seeded = true;
    }

    /* One fill: one pid check and one budget charge for the whole
     * block.  After a fork, that pid check re-keys the state. */
    portableRandomFill64(&t->prs, t->block, DK_RANDOM_BLOCK_WORDS);

    t->blockUsed = 0;
    t->blockGeneration = generation;
}

uint64_t dkRandom64(void) {
    dkRandomThread *const t = &dkRandomTls;
    const uint64_t generation =
        atomic_load_explicit(&forkGeneration, memory_order_relaxed);

    if (unlikely(t->blockUsed == DK_RANDOM_BLOCK_WORDS ||
                 generation != t->blockGeneration)) {
        dkRandomRefill(t, generation);
    }

    return t->block[t->blockUsed++];
}

uint64_t dkRandomBounded(const uint64_t bound) {
    if (bound == 0) {
        (void)atomic_fetch_add_explicit(&zeroBoundTotal, 1,
                                        memory_order_relaxed);
        return 0;
    }

    if (bound == 1) {
        return 0;
    }

    /* Lemire's nearly divisionless method: the high half of x * bound is
     * the result and the low half is x's offset within its class.  Draws
     * whose low half is below 2^64 mod bound are rejected, which makes
     * all classes the same size, so the result is exactly uniform.  The
     * remainder (a division) is computed only when low < bound, which is
     * rare unless bound approaches 2^64. */
    __uint128_t wide = (__uint128_t)dkRandom64() * (__uint128_t)bound;
    uint64_t low = (uint64_t)wide;

    if (low < bound) {
        /* 2^64 mod bound, written without a unary minus on an unsigned
         * value: bound >= 2 here, so neither term can wrap. */
        const uint64_t threshold = (UINT64_MAX - bound + 1) % bound;

        while (low < threshold) {
            wide = (__uint128_t)dkRandom64() * (__uint128_t)bound;
            low = (uint64_t)wide;
        }
    }

    return (uint64_t)(wide >> DK_RANDOM_WORD_BITS);
}

uint64_t dkRandomZeroBoundTotal(void) {
    return atomic_load_explicit(&zeroBoundTotal, memory_order_relaxed);
}

#ifdef DATAKIT_TEST
#include "ctest.h"
#include "datakit.h"

#include <sys/wait.h>

/* Advance this thread's block so the next dkRandom64() call refills
 * (atBoundary) or reads the second word of a fresh block. */
static void dkRandomTestPositionBlock(const bool atBoundary) {
    (void)dkRandom64(); /* seeds the thread on first use */
    while (dkRandomTls.blockUsed != DK_RANDOM_BLOCK_WORDS) {
        (void)dkRandom64();
    }

    if (!atBoundary) {
        (void)dkRandom64();
    }
}

/* Fork; draw one dkRandom64() word in the child (sent back over a pipe)
 * and one in the parent.  Returns false if pipe/fork/read failed.  The
 * child _exits so it runs no atexit or sanitizer handlers. */
static bool dkRandomTestForkedNextWord(uint64_t *childWord,
                                       uint64_t *parentWord) {
    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }

    const pid_t kid = fork();
    if (kid < 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return false;
    }

    if (kid == 0) {
        (void)close(fds[0]);
        const uint64_t word = dkRandom64();
        const ssize_t wrote = write(fds[1], &word, sizeof(word));
        (void)close(fds[1]);
        _exit(wrote == (ssize_t)sizeof(word) ? 0 : 1);
    }

    (void)close(fds[1]);
    *parentWord = dkRandom64();

    uint64_t got = 0;
    size_t have = 0;
    while (have < sizeof(got)) {
        const ssize_t n =
            read(fds[0], (uint8_t *)&got + have, sizeof(got) - have);
        if (n <= 0) {
            break;
        }

        have += (size_t)n;
    }

    (void)close(fds[0]);

    int status = 0;
    (void)waitpid(kid, &status, 0);

    if (have != sizeof(got)) {
        return false;
    }

    *childWord = got;
    return true;
}

int dkRandomTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("dkRandom64 gives every bit both values") {
        /* Every bit of dkRandom64 must be seen both set and clear in 128
         * draws (false-positive bound 2*64*2^-128). */
        const uint64_t draws = 128;

        uint64_t everSet = 0;
        uint64_t everClear = 0;
        for (uint64_t d = 0; d < draws; d++) {
            const uint64_t v = dkRandom64();
            everSet |= v;
            everClear |= ~v;
        }

        for (uint32_t b = 0; b < (uint32_t)DK_RANDOM_WORD_BITS; b++) {
            const uint64_t mask = UINT64_C(1) << b;

            if (!(everSet & mask)) {
                ERR("bit %" PRIu32 " was never set across %" PRIu64 " draws", b,
                    draws);
            }

            if (!(everClear & mask)) {
                ERR("bit %" PRIu32 " was never clear across %" PRIu64 " draws",
                    b, draws);
            }
        }
    }

    TEST("dkRandom64 gives no bit a lopsided share") {
        /* Each bit's set count over 4096 draws must lie within +-256
         * (8 sd) of 2048; Hoeffding bound per bit 2e^-32, union over 64
         * bits < 2e-12. */
        const uint64_t draws = 4096;
        const uint64_t band = draws / 16;
        const uint64_t centre = draws / 2;

        uint64_t setCount[DK_RANDOM_WORD_BITS] = {0};
        for (uint64_t d = 0; d < draws; d++) {
            const uint64_t v = dkRandom64();
            for (uint32_t b = 0; b < (uint32_t)DK_RANDOM_WORD_BITS; b++) {
                setCount[b] += (v >> b) & UINT64_C(1);
            }
        }

        for (uint32_t b = 0; b < (uint32_t)DK_RANDOM_WORD_BITS; b++) {
            const uint64_t s = setCount[b];
            const uint64_t deviation = s > centre ? s - centre : centre - s;

            if (deviation > band) {
                ERR("bit %" PRIu32 " was set %" PRIu64 " times in %" PRIu64
                    " draws (centre %" PRIu64 ", band %" PRIu64 ")",
                    b, s, draws, centre, band);
            }
        }
    }

    TEST("dkRandom64 can emit every byte value") {
        /* Every byte value must appear in 32768 output bytes; catches a
         * generator state that is not a permutation of 0..255 (union
         * bound 256*e^-128). */
        const uint64_t draws = 4096;
        const uint32_t alphabet = (uint32_t)UINT8_MAX + 1;

        bool seen[(uint32_t)UINT8_MAX + 1] = {false};
        for (uint64_t d = 0; d < draws; d++) {
            uint64_t v = dkRandom64();
            for (uint32_t k = 0; k < sizeof(v); k++) {
                seen[v & UINT8_MAX] = true;
                v >>= CHAR_BIT;
            }
        }

        uint32_t unreachable = 0;
        uint32_t firstUnreachable = 0;
        for (uint32_t value = 0; value < alphabet; value++) {
            if (!seen[value]) {
                if (unreachable == 0) {
                    firstUnreachable = value;
                }

                unreachable++;
            }
        }

        if (unreachable > 0) {
            ERR("%" PRIu32 " of %" PRIu32 " byte values were never emitted "
                "in %" PRIu64 " bytes (first: %" PRIu32 ")",
                unreachable, alphabet, draws * sizeof(uint64_t),
                firstUnreachable);
        }
    }

    TEST("dkRandomBounded reaches the upper half of a 2^32 bound") {
        /* dkRandomBounded(2^32) must reach [2^31, 2^32) within 64 draws
         * (false-positive 2^-64). */
        const uint64_t bound = UINT64_C(1) << 32;
        const uint64_t upperHalf = bound / 2;
        const uint64_t draws = 64;

        bool reachedUpper = false;
        uint64_t widest = 0;
        for (uint64_t d = 0; d < draws && !reachedUpper; d++) {
            const uint64_t v = dkRandomBounded(bound);

            if (v >= bound) {
                ERR("draw %" PRIu64 " returned %" PRIu64 ", outside "
                    "[0, %" PRIu64 ")",
                    d, v, bound);
                break;
            }

            if (v > widest) {
                widest = v;
            }

            reachedUpper = v >= upperHalf;
        }

        if (!reachedUpper) {
            ERR("no draw of %" PRIu64 " reached [%" PRIu64 ", %" PRIu64 ") "
                "(widest %" PRIu64 ")",
                draws, upperHalf, bound, widest);
        }
    }

    TEST("dkRandomBounded's contract holds at awkward bounds") {
        /* Results stay below the bound at edge bounds, including values
         * near 2^31, 2^32 and 2^62+1 (rejection probability ~1/4, which
         * exercises the rejection loop) and UINT64_MAX.  bound 1 and
         * bound 0 return 0; bound 0 is counted. */
        const uint64_t bounds[] = {
            1,
            2,
            3,
            7,
            (UINT64_C(1) << 31) - 1,
            UINT64_C(1) << 31,
            (UINT64_C(1) << 31) + 1,
            (UINT64_C(1) << 32) - 1,
            UINT64_C(1) << 32,
            (UINT64_C(1) << 32) + 1,
            (UINT64_C(1) << 62) + 1,
            UINT64_MAX,
        };
        const uint64_t drawsPerBound = 1024;

        for (size_t i = 0; i < COUNT_ARRAY(bounds); i++) {
            const uint64_t bound = bounds[i];

            for (uint64_t d = 0; d < drawsPerBound; d++) {
                const uint64_t v = dkRandomBounded(bound);

                if (v >= bound) {
                    ERR("dkRandomBounded(%" PRIu64 ") returned %" PRIu64
                        " on draw %" PRIu64,
                        bound, v, d);
                    break;
                }
            }
        }

        if (dkRandomBounded(1) != 0) {
            ERRR("dkRandomBounded(1) must return 0");
        }

        /* bound 0 returns 0 without dividing and increments the tally. */
        const uint64_t zeroBefore = dkRandomZeroBoundTotal();

        if (dkRandomBounded(0) != 0) {
            ERRR("dkRandomBounded(0) must return 0");
        }

        const uint64_t zeroAfter = dkRandomZeroBoundTotal();
        if (zeroAfter != zeroBefore + 1) {
            ERR("the zero-bound tally moved from %" PRIu64 " to %" PRIu64
                ", not by one",
                zeroBefore, zeroAfter);
        }
    }

    TEST("a forked child does not repeat the parent's next word") {
        /* At a block boundary both sides refill, and the child's refill
         * re-keys because the state was seeded with the real pid.  In
         * the middle of a block the child must also discard the block
         * it inherited.  Either way a child word equal to the parent's
         * means the child replayed the parent's stream (false-positive
         * 2^-64 per case). */
        for (uint32_t c = 0; c < 2; c++) {
            const bool atBoundary = c == 0;
            dkRandomTestPositionBlock(atBoundary);

            uint64_t childWord = 0;
            uint64_t parentWord = 0;
            if (!dkRandomTestForkedNextWord(&childWord, &parentWord)) {
                ERRR("the fork probe could not run (pipe/fork/read failed)");
            } else if (childWord == parentWord) {
                ERR("%s: a forked child drew 0x%016" PRIx64 ", the same "
                    "word as the parent",
                    atBoundary ? "block boundary" : "mid-block", childWord);
            }
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
