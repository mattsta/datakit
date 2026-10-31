#include "dkRandom.h"

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include "datakit.h"
#include "dkDiag.h"
#include "portableRandom.h"

/* Words generated per refill.  Each drawing thread carries this block
 * (512 bytes) beside its portableRandomState.  A refill is one
 * portableRandomFill64 call, so the pid check (a syscall on libcs that
 * do not cache the pid) and the refuel budget charge run once per block
 * rather than once per word, and a draw is a TLS read. */
static constexpr int DK_RANDOM_BLOCK_WORDS = 64;

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
    /* @sync: relaxed init */
    (void)atomic_fetch_add_explicit(&forkGeneration, 1, memory_order_relaxed);
}

static void dkRandomForkHandlerRegister(void) {
    (void)pthread_atfork(nullptr, nullptr, dkRandomForkChild);
}

/* Per-thread generator and output block, in one struct so a draw
 * resolves one TLS address (on Darwin each thread_local variable is
 * its own thunk call).  The fields a draw reads come first.  blockUsed
 * starts full so a thread's first draw refills. */
typedef struct dkRandomThread {
    uint32_t blockUsed;
    bool seeded;
    uint64_t blockGeneration;
    uint64_t block[DK_RANDOM_BLOCK_WORDS];
    portableRandomState prs;
} dkRandomThread;

static thread_local dkRandomThread dkRandomTls = {.blockUsed =
                                                      DK_RANDOM_BLOCK_WORDS};

#ifdef DATAKIT_TEST
/* dkRandomTestReplay's stream, read only by a refill */
static thread_local bool dkRandomReplaying_;
static thread_local uint64_t dkRandomReplayState_;
#endif

/* Out of line so the draw path stays a load, a compare and a return. */
[[gnu::noinline]] static void dkRandomRefill(dkRandomThread *const t,
                                             const uint64_t generation) {
#ifdef DATAKIT_TEST
    if (dkRandomReplaying_) {
        for (uint32_t i = 0; i < DK_RANDOM_BLOCK_WORDS; i++) {
            t->block[i] = dkSplitmix64Next(&dkRandomReplayState_);
        }
        t->blockUsed = 0;
        t->blockGeneration = generation;
        return;
    }
#endif

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
    /* Every increment ran while the process had one thread, so it
     * happens-before this thread's creation or is this thread's own. */
    /* @sync: relaxed init */
    const uint64_t generation =
        atomic_load_explicit(&forkGeneration, memory_order_relaxed);

    if (unlikely(t->blockUsed == DK_RANDOM_BLOCK_WORDS ||
                 generation != t->blockGeneration)) {
        dkRandomRefill(t, generation);
    }

    return t->block[t->blockUsed++];
}

/* dkRandom64 in the shape of a stream step, for dkBoundedFrom */
static uint64_t dkRandomThreadDraw(uint64_t *) {
    return dkRandom64();
}

/* A value in [0, bound) from the stream 'draw' advances at '*state'; the
 * one bounded reduction behind dkRandomBounded and dkSplitmix64Bounded.
 * Always inlined, so each caller's draw is a direct call. */
[[gnu::always_inline]] static inline uint64_t
dkBoundedFrom(uint64_t (*const draw)(uint64_t *), uint64_t *const state,
              const uint64_t bound) {
    if (bound == 0) {
        /* @sync: relaxed counter */
        (void)atomic_fetch_add_explicit(&zeroBoundTotal, 1,
                                        memory_order_relaxed);
        DK_DIAG(DK_DIAG_RANDOM_ZERO_BOUND,
                "a value from the empty interval [0, 0); returning 0");
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
    __uint128_t wide = (__uint128_t)draw(state) * (__uint128_t)bound;
    uint64_t low = (uint64_t)wide;

    if (low < bound) {
        /* 2^64 mod bound, written without a unary minus on an unsigned
         * value: bound >= 2 here, so neither term can wrap. */
        const uint64_t threshold = (UINT64_MAX - bound + 1) % bound;

        while (low < threshold) {
            wide = (__uint128_t)draw(state) * (__uint128_t)bound;
            low = (uint64_t)wide;
        }
    }

    return (uint64_t)(wide >> DK_RANDOM_WORD_BITS);
}

uint64_t dkRandomBounded(const uint64_t bound) {
    return dkBoundedFrom(dkRandomThreadDraw, nullptr, bound);
}

uint64_t dkSplitmix64Bounded(uint64_t *const state, const uint64_t bound) {
    return dkBoundedFrom(dkSplitmix64Next, state, bound);
}

uint64_t dkRandomZeroBoundTotal(void) {
    /* @sync: relaxed counter */
    return atomic_load_explicit(&zeroBoundTotal, memory_order_relaxed);
}

#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"

#include <sys/wait.h>

defcleanerName(void *, zfree, Void);

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

/* A replay or its end takes effect at the next draw: the block is
 * refilled */
void dkRandomTestReplay(const uint64_t seed) {
    dkRandomReplaying_ = true;
    dkRandomReplayState_ = seed;
    dkRandomTls.blockUsed = DK_RANDOM_BLOCK_WORDS;
}

void dkRandomTestLive(void) {
    dkRandomReplaying_ = false;
    dkRandomTls.blockUsed = DK_RANDOM_BLOCK_WORDS;
}

int dkRandomTest(int, char *[]) {
    int err = 0;

    TEST("dkRandomTestReplay repeats a run's draws; Live ends it") {
        uint64_t first[DK_RANDOM_BLOCK_WORDS + 3];
        uint64_t again[DK_RANDOM_BLOCK_WORDS + 3];
        uint64_t state = 99;
        dkRandomTestReplay(99);
        for (size_t i = 0; i < countof(first); i++) {
            first[i] = dkRandom64();
        }
        dkRandomTestReplay(99);
        for (size_t i = 0; i < countof(again); i++) {
            again[i] = dkRandom64();
        }
        dkRandomTestLive();
        for (size_t i = 0; i < countof(first); i++) {
            if (first[i] != again[i] || first[i] != dkSplitmix64Next(&state)) {
                ERR("replayed draw %zu differs", i);
            }
        }
        /* Live again: not the replay's next value */
        if (dkRandom64() == dkSplitmix64Next(&state)) {
            ERR("%s", "the live generator gave the replay's next draw");
        }
    }

    TEST("dkXorshift64Next / dkXorshift32Next replay Marsaglia's xorshift") {
        /* From 1, worked by hand: 1 ^ 1<<13 = 8193; ^ >>7 = 8257;
         * ^ <<17 = 1082269761.  32-bit: 8193; ^ >>17 = 8193; ^ <<5 =
         * 270369. */
        uint64_t s64 = 1;
        uint32_t s32 = 1;
        if (dkXorshift64Next(&s64) != UINT64_C(1082269761) ||
            s64 != UINT64_C(1082269761)) {
            ERR("xorshift64 from 1 gave %" PRIu64, s64);
        }
        if (dkXorshift32Next(&s32) != 270369u || s32 != 270369u) {
            ERR("xorshift32 from 1 gave %" PRIu32, s32);
        }
    }

    TEST("dkXorshift64StarNext replays Vigna's xorshift64*") {
        /* From 1, worked by hand: 1 ^ 1>>12 = 1; ^ <<25 = 33554433;
         * ^ >>27 = 33554433.  The output is that state times the
         * multiplier, modulo 2^64. */
        uint64_t s = 1;
        const uint64_t out = dkXorshift64StarNext(&s);
        if (s != UINT64_C(33554433) ||
            out != UINT64_C(33554433) * UINT64_C(0x2545F4914F6CDD1D)) {
            ERR("xorshift64* from 1 gave state %" PRIu64, s);
        }
    }

    TEST("dkSplitmix64Next replays Vigna's splitmix64") {
        /* From 0, the reference generator's first two outputs; the state
         * advances by the golden-ratio step each time */
        uint64_t state = 0;
        const uint64_t first = dkSplitmix64Next(&state);
        const uint64_t second = dkSplitmix64Next(&state);
        if (first != UINT64_C(16294208416658607535) ||
            second != UINT64_C(7960286522194355700) ||
            state != 2 * UINT64_C(0x9E3779B97F4A7C15)) {
            ERR("from 0: %" PRIu64 ", %" PRIu64 ", state %" PRIu64, first,
                second, state);
        }
    }

    TEST("dkSplitmix64Bounded replays the splitmix64 draws it reduces") {
        /* From seed 0: the high word of each reference draw times the
         * bound.  The first draw is splitmix64's published first output;
         * a rejection at bound 1000 needs a low word below 616, which
         * these draws do not have. */
        uint64_t state = 0;
        uint64_t ref = 0;
        for (int i = 0; i < 64; i++) {
            const uint64_t draw = dkSplitmix64Next(&ref);
            if (i == 0 && draw != UINT64_C(16294208416658607535)) {
                ERR("reference first draw %" PRIu64, draw);
            }
            const uint64_t want =
                (uint64_t)(((unsigned __int128)draw * 1000u) >> 64);
            const uint64_t got = dkSplitmix64Bounded(&state, 1000);
            if (got != want || state != ref) {
                ERR("draw %d: %" PRIu64 ", want %" PRIu64, i, got, want);
                break;
            }
        }

        /* Every bound is reachable: the top bit of a draw below
         * UINT64_MAX shows up within 64 draws (a miss is 2^-64) */
        bool high = false;
        for (int i = 0; i < 64 && !high; i++) {
            high = dkSplitmix64Bounded(&state, UINT64_MAX) >> 63;
        }
        if (!high) {
            ERR("no draw below UINT64_MAX reached 2^63");
        }

        const uint64_t zeros = dkRandomZeroBoundTotal();
        const uint64_t before = state;
        if (dkSplitmix64Bounded(&state, 0) != 0 || state != before ||
            dkRandomZeroBoundTotal() != zeros + 1) {
            ERR("bound 0 must return 0, leave the state, and be counted");
        }
    }

    TEST("dkLcg64Next replays the MMIX LCG") {
        /* From 0 the first value is the increment; then each step matches
         * the recurrence computed in 128 bits and reduced mod 2^64 */
        uint64_t state = 0;
        if (dkLcg64Next(&state) != UINT64_C(1442695040888963407) ||
            state != UINT64_C(1442695040888963407)) {
            ERR("first value from 0 is %" PRIu64, state);
        }
        for (int i = 0; i < 1000; i++) {
            const uint64_t want =
                (uint64_t)((unsigned __int128)state * 6364136223846793005u +
                           1442695040888963407u);
            if (dkLcg64Next(&state) != want || state != want) {
                ERR("step %d: %" PRIu64 ", want %" PRIu64, i, state, want);
                break;
            }
        }
    }

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

        uint64_t setCount[DK_RANDOM_WORD_BITS] = {};
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

        for (size_t i = 0; i < countof(bounds); i++) {
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
            ERR("dkRandomBounded(1) must return 0");
        }

        /* bound 0 returns 0 without dividing, increments the tally and
         * reports one misuse on this thread. */
        const uint64_t zeroBefore = dkRandomZeroBoundTotal();
        const uint64_t diagBefore = dkDiagCount();

        if (dkRandomBounded(0) != 0) {
            ERR("dkRandomBounded(0) must return 0");
        }

        dkDiagnostic d;
        if (dkDiagCount() != diagBefore + 1 || !dkDiagLast(&d) ||
            d.code != DK_DIAG_RANDOM_ZERO_BOUND) {
            ERR("dkRandomBounded(0) must report RANDOM_ZERO_BOUND once");
        }

        const uint64_t zeroAfter = dkRandomZeroBoundTotal();
        if (zeroAfter != zeroBefore + 1) {
            ERR("the zero-bound tally moved from %" PRIu64 " to %" PRIu64
                ", not by one",
                zeroBefore, zeroAfter);
        }
    }

    TEST("dkRandomBounded is exactly uniform where rejection matters") {
        /* bound = 0xAAAAAAAAAAAAAAAB is about 2^64 / 1.5, so without
         * rejection the multiply-shift maps one source value to some
         * results and two to others, alternating with the result's
         * parity: odd results would come out about 1/3 of the time, not
         * 1/2.  Rejection discards about 1/3 of draws to remove that.
         * Over 4096 draws the odd share must lie in [0.40, 0.60]
         * (Hoeffding false-failure bound 2e^-81.92, about 4e-36; the
         * no-rejection share sits about 20 sd below the band). */
        const uint64_t bound = UINT64_C(0xAAAAAAAAAAAAAAAB);
        const uint64_t draws = 4096;
        const uint64_t lowest = draws * 2 / 5;
        const uint64_t highest = draws * 3 / 5;

        uint64_t odd = 0;
        for (uint64_t d = 0; d < draws; d++) {
            const uint64_t v = dkRandomBounded(bound);

            if (v >= bound) {
                ERR("dkRandomBounded(0x%016" PRIx64 ") returned 0x%016" PRIx64,
                    bound, v);
                break;
            }

            odd += v & UINT64_C(1);
        }

        if (odd < lowest || odd > highest) {
            ERR("dkRandomBounded(0x%016" PRIx64 ") gave %" PRIu64 " odd "
                "results in %" PRIu64 " draws, outside [%" PRIu64 ", %" PRIu64
                "]",
                bound, odd, draws, lowest, highest);
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
                ERR("the fork probe could not run (pipe/fork/read failed)");
            } else if (childWord == parentWord) {
                ERR("%s: a forked child drew 0x%016" PRIx64 ", the same "
                    "word as the parent",
                    atBoundary ? "block boundary" : "mid-block", childWord);
            }
        }
    }

    TEST("ctestReplayBytes lays splitmix64 draws out little-endian") {
        /* From seed 0 the first draw is splitmix64's published first
         * output; its bytes come low first, and a 3-byte tail takes the
         * low bytes of the next draw */
        uint8_t got[11];
        uint64_t state = 0;
        ctestReplayBytes(got, sizeof(got), &state);

        uint64_t ref = 0;
        const uint64_t first = dkSplitmix64Next(&ref);
        const uint64_t second = dkSplitmix64Next(&ref);
        uint8_t want[11];
        for (size_t i = 0; i < 8; i++) {
            want[i] = (uint8_t)(first >> (8 * i));
        }
        for (size_t i = 0; i < 3; i++) {
            want[8 + i] = (uint8_t)(second >> (8 * i));
        }

        TEST_BYTES(err, got, want, sizeof(want), "11 bytes from seed 0");
        if (first != UINT64_C(16294208416658607535)) {
            ERR("reference first draw %" PRIu64, first);
        }

        if (state != ref) {
            ERR("11 bytes took other than two draws");
        }

        const uint64_t before = state;
        ctestReplayBytes(got, 0, &state);
        if (state != before) {
            ERR("an empty fill drew");
        }
    }

    TEST("ctestHeapMeter counts usable bytes live between start and stop") {
        if (!datakitConfigMemory__.localUsableSize) {
            printf("(skipped: the allocator reports no usable sizes)\n");
        } else {
            /* A block from before the start, grown inside: only the
             * growth counts */
            cleanerName(zfree, Void) void *before = zmalloc(200);
            const int64_t beforeSize = (int64_t)zusablesize(before);

            if (!ctestHeapMeterStart()) {
                ERR("the meter did not start");
            }

            if (ctestHeapMeterStart()) {
                ERR("a second meter started while one ran");
            }

            cleanerName(zfree, Void) void *kept = zcalloc(1, 1000);
            {
                /* Allocated and freed inside: nets to nothing */
                cleanerName(zfree, Void) void *passing = zmalloc(300);
                if (zusablesize(passing) < 300) {
                    ERR("a 300-byte block has %zu usable",
                        zusablesize(passing));
                }
            }

            zreallocSelf(before, 5000);
            const int64_t want =
                (int64_t)(zusablesize(kept) + zusablesize(before)) - beforeSize;
            const int64_t metered = ctestHeapMeterStop();

            if (zusablesize(kept) < 1000 || metered != want) {
                ERR("metered %" PRId64 ", want %" PRId64
                    " (kept block %zu, grown block %zu from %" PRId64 ")",
                    metered, want, zusablesize(kept), zusablesize(before),
                    beforeSize);
            }

            if (ctestHeapMeterStop() != 0) {
                ERR("a stop with no meter running reported bytes");
            }

            /* Restored: the frees at scope end go straight to the
             * allocator */
            if (datakitConfigMemory__.localFree == ctestHeapFree_) {
                ERR("the meter left its allocator installed");
            }
        }
    }

    TEST("ctestRandomBytes fills exactly 'len' bytes, 64 KiB then copies") {
        constexpr size_t fresh = 64 * 1024;
        constexpr size_t guard = 16;
        static uint8_t buf[3 * fresh + 3 + guard];
        const size_t lens[] = {0,
                               1,
                               7,
                               8,
                               9,
                               fresh - 1,
                               fresh,
                               fresh + 1,
                               2 * fresh,
                               3 * fresh,
                               3 * fresh + 3};
        for (size_t l = 0; l < countof(lens); l++) {
            const size_t len = lens[l];
            memset(buf, 0xA5, sizeof(buf));
            ctestRandomBytes(buf, len);

            for (size_t i = len; i < len + guard; i++) {
                if (buf[i] != 0xA5) {
                    ERR("len %zu: byte %zu past the end was written", len, i);
                    break;
                }
            }

            for (size_t i = fresh; i < len; i++) {
                if (buf[i] != buf[i % fresh]) {
                    ERR("len %zu: byte %zu is not a copy of byte %zu", len, i,
                        i % fresh);
                    break;
                }
            }

            /* 16 or more drawn bytes all equal to the poison: 2^-128 */
            if (len >= 16) {
                size_t poisoned = 0;
                while (poisoned < len && buf[poisoned] == 0xA5) {
                    poisoned++;
                }

                if (poisoned == len) {
                    ERR("len %zu: no byte was drawn", len);
                }
            }
        }
    }

    TEST("DK_SHUFFLE: a permutation, replayable, every order about as "
         "likely") {
        /* 1000 elements: each once after the shuffle, and the same seed
         * gives the same order */
        uint32_t a[1000];
        uint32_t b[1000];
        for (uint32_t i = 0; i < 1000; i++) {
            a[i] = b[i] = i;
        }
        uint64_t seedA = 42;
        uint64_t seedB = 42;
        DK_SHUFFLE(a, 1000, &seedA);
        DK_SHUFFLE(b, 1000, &seedB);
        bool seen[1000] = {};
        size_t moved = 0;
        for (uint32_t i = 0; i < 1000; i++) {
            if (a[i] >= 1000 || seen[a[i]] || a[i] != b[i]) {
                ERR("element %u: %u (replay %u)", i, a[i], b[i]);
                break;
            }
            seen[a[i]] = true;
            moved += a[i] != i;
        }
        if (moved < 900) {
            ERR("only %zu of 1000 elements moved", moved);
        }

        /* Three elements, 60000 shuffles: each of the six orders about a
         * sixth of the time (within 5%) */
        size_t counts[6] = {};
        uint64_t seed = 7;
        for (int round = 0; round < 60000; round++) {
            int three[3] = {0, 1, 2};
            DK_SHUFFLE(three, 3, &seed);
            counts[three[0] * 2 + (three[1] > three[2])]++;
        }
        for (size_t k = 0; k < 6; k++) {
            if (counts[k] < 9500 || counts[k] > 10500) {
                ERR("order %zu came %zu times in 60000", k, counts[k]);
            }
        }

        /* Zero and one element: nothing to do */
        int one[1] = {9};
        DK_SHUFFLE(one, 1, &seed);
        DK_SHUFFLE(one, 0, &seed);
        if (one[0] != 9) {
            ERR("a one-element shuffle changed it");
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
