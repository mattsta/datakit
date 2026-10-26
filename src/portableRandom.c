/* Originally from njs/nxt nginx BSD license */

#include "datakit.h"

#include <sys/types.h>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#if DK_HAVE_GETRANDOM
#include <sys/random.h>
#elif DK_HAVE_LINUX_SYS_GETRANDOM
#include <linux/random.h>
#include <sys/syscall.h>
#elif DK_HAVE_GETENTROPY_SYS_RANDOM
#include <sys/random.h>
#endif

#include "portableRandom.h"

/*
 * The pseudorandom generator based on OpenBSD arc4random.  Although
 * it is usually stated that arc4random uses RC4 pseudorandom generation
 * algorithm they are actually different in portableRandom_add().
 */

#define DK_PR_KEYSIZE 128

DK_INLINE_ALWAYS uint64_t portableRandom_byte(portableRandomState *r) {
    r->i++;
    uint8_t si = r->s[r->i];
    r->j += si;

    const uint8_t sj = r->s[r->j];
    r->s[r->i] = sj;
    r->s[r->j] = si;

    si += sj;

    return r->s[si];
}

void portableRandom_init(portableRandomState *r, pid_t pid) {
    r->remainingBytesBeforeRefuel = 0;
    r->pid = pid;
    r->i = 0;
    r->j = 0;

    /* s[] must remain a permutation of 0..255: RC4 outputs s[] entries,
     * so a missing value can never be emitted and a duplicate is
     * over-represented. */
    for (size_t i = 0; i < sizeof(r->s); i++) {
        r->s[i] = i;
    }

    /* Mix OS entropy in through the key schedule, which only swaps
     * entries and so preserves the permutation (writing entropy straight
     * into s[] would break it permanently, since later stirs only swap).
     * The key is zeroed first, so a short read still keys the state.
     *
     * The first draw re-keys through portableRandom_stir anyway, but
     * this read is not redundant.  A stir permutes the current state
     * rather than replacing it, so this key survives every later stir.
     * If all of a stir's OS sources fail, the stir keys from the time
     * and pid alone, and this key is then the only OS entropy in the
     * state.  The read costs one syscall per state. */
    uint8_t key[DK_PR_KEYSIZE] = {};
    if (portableRandomDirect(key, sizeof(key)) > 0) {
        portableRandom_add(r, key, sizeof(key));
    }
}

/* getentropy() rejects requests over 256 bytes, so full fills are
 * chunked at that size on every arm. */
#define DK_PR_DIRECT_MAX_CHUNK_BYTES 256

/* Read up to `len` bytes from /dev/urandom, retrying reads that a signal
 * interrupts.  Returns the bytes read, or -1 if nothing could be read.
 * O_CLOEXEC keeps the descriptor out of any child that another thread
 * fork+execs while it is open.  On Linux /dev/urandom never blocks, even
 * before the kernel pool is initialised. */
static ssize_t portableRandomUrandom(void *const dst, const size_t len) {
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    uint8_t *out = dst;
    size_t got = 0;
    while (got < len) {
        const ssize_t n = read(fd, out + got, len - got);
        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            break;
        }

        got += (size_t)n;
    }

    (void)close(fd);
    return got > 0 || len == 0 ? (ssize_t)got : -1;
}

ssize_t portableRandomDirect(void *const dst, const size_t requestedLen) {
    /* "1" means GRND_NONBLOCK. If no entropy and WOULDBLOCK, -1 is returned
     * with EAGAIN. */
    [[maybe_unused]] const uint32_t DK_GRND_NONBLOCK = 1;

#if DK_HAVE_GETRANDOM
    return getrandom(dst, requestedLen, DK_GRND_NONBLOCK);
#elif DK_HAVE_LINUX_SYS_GETRANDOM
    /* Linux 3.17 SYS_getrandom, not available in Glibc prior to 2.25. */
    return syscall(SYS_getrandom, dst, requestedLen, DK_GRND_NONBLOCK);
#elif DK_HAVE_GETENTROPY || DK_HAVE_GETENTROPY_SYS_RANDOM
    /* getentropy() returns 0 for a complete fill; normalise to the
     * bytes-written contract. */
    return getentropy(dst, requestedLen) == 0 ? (ssize_t)requestedLen : -1;
#else
    /* No OS call was detected at build time (detection runs test
     * programs, so a cross build detects none), so read the device. */
    return portableRandomUrandom(dst, requestedLen);
#endif
}

bool portableRandomDirectFull(void *const dst, const size_t requestedLen) {
    uint8_t *out = dst;
    size_t filled = 0;

    while (filled < requestedLen) {
        size_t want = requestedLen - filled;
        if (want > DK_PR_DIRECT_MAX_CHUNK_BYTES) {
            want = DK_PR_DIRECT_MAX_CHUNK_BYTES;
        }

        const ssize_t n = portableRandomDirect(out + filled, want);

        /* A non-positive answer is a failure, not a retry, so a source
         * with nothing to give cannot hang the caller. */
        if (n <= 0) {
            return false;
        }

        filled += (size_t)n;
    }

    return true;
}

void portableRandom_stir(portableRandomState *r, pid_t pid) {
    ssize_t n;
    struct timeval tv;
    union {
        uint32_t value[3];
        uint8_t bytes[DK_PR_KEYSIZE];
    } key;

    if (r->pid == 0) {
        portableRandom_init(r, pid);
    }

    r->pid = pid;

    /* A stir must not wait for the kernel pool.  getrandom without
     * GRND_NONBLOCK waits for it to initialise, which can take a long
     * time early in boot, and any draw can stir.  Before the pool is ready the
     * non-blocking call fails with EAGAIN, and the key comes from
     * /dev/urandom instead, then from the time/pid mix below.  Key
     * material must come from portableRandomDirectFull, which fails
     * rather than return bytes from an uninitialised pool. */
#if DK_HAVE_GETRANDOM
    n = getrandom(&key.bytes, DK_PR_KEYSIZE, GRND_NONBLOCK);
#elif DK_HAVE_LINUX_SYS_GETRANDOM
    /* Linux 3.17 SYS_getrandom, not available in Glibc prior to 2.25. */
    n = syscall(SYS_getrandom, &key.bytes, DK_PR_KEYSIZE, GRND_NONBLOCK);
#elif DK_HAVE_GETENTROPY || DK_HAVE_GETENTROPY_SYS_RANDOM
    n = 0;
    if (getentropy(&key.bytes, DK_PR_KEYSIZE) == 0) {
        n = DK_PR_KEYSIZE;
    }
#else
    n = 0;
#endif

    if (n != DK_PR_KEYSIZE) {
        n = portableRandomUrandom(&key.bytes, DK_PR_KEYSIZE);
    }

    if (n != DK_PR_KEYSIZE) {
        (void)gettimeofday(&tv, NULL);

        /* XOR with stack garbage. */

        key.value[0] ^= tv.tv_usec;
        key.value[1] ^= tv.tv_sec;
        key.value[2] ^= getpid();
    }

    portableRandom_add(r, key.bytes, DK_PR_KEYSIZE);

    /* Drop the first 3072 bytes. */
    for (size_t i = 0; i < 3072; i++) {
        (void)portableRandom_byte(r);
    }

    /* Re-key from the OS after DK_PR_REFUEL_BYTES bytes. */
    r->remainingBytesBeforeRefuel = DK_PR_REFUEL_BYTES;
}

void portableRandom_add(portableRandomState *r, const uint8_t *key,
                        size_t len) {
    /* An empty key has nothing to mix in, and key[n % len] below would
     * divide by zero. */
    if (len == 0) {
        return;
    }

    for (uint32_t n = 0; n < sizeof(r->s); n++) {
        uint8_t val = r->s[r->i];
        r->j += val + key[n % len];

        r->s[r->i] = r->s[r->j];
        r->s[r->j] = val;

        r->i++;
    }

    /* This index is not decremented in RC4 algorithm. */
    r->i--;

    r->j = r->i;
}

#define gen32()                                                                \
    do {                                                                       \
        val = portableRandom_byte(r) << 24;                                    \
        val |= portableRandom_byte(r) << 16;                                   \
        val |= portableRandom_byte(r) << 8;                                    \
        val |= portableRandom_byte(r);                                         \
    } while (0)

#define genWidth(val)                                                          \
    do {                                                                       \
        val = 0;                                                               \
        for (size_t i = 1; i <= sizeof(val); i++) {                            \
            /* this weird typeof cast is because _byte() returns smaller than  \
             * __uint128_t we try to shift against sometimes. */               \
            val |= (typeof(val))portableRandom_byte(r)                         \
                   << ((sizeof(val) * 8) - (8 * i));                           \
        }                                                                      \
    } while (0)

/* The pid checks are assertations aginst RNG problems in forked processes. */
#define randomSetup(content)                                                   \
    do {                                                                       \
        new_pid = 0;                                                           \
        pid = r->pid;                                                          \
                                                                               \
        if (pid != -1) {                                                       \
            pid = getpid();                                                    \
                                                                               \
            if (pid != r->pid) {                                               \
                new_pid = 1;                                                   \
            }                                                                  \
        }                                                                      \
                                                                               \
        r->remainingBytesBeforeRefuel -= sizeof(content);                      \
                                                                               \
        if (r->remainingBytesBeforeRefuel <= 0 || new_pid) {                   \
            portableRandom_stir(r, pid);                                       \
        }                                                                      \
    } while (0)

uint32_t portableRandom32(portableRandomState *r) {
    uint32_t val;
    pid_t pid;
    bool new_pid;

    randomSetup(val);
    gen32();

    return val;
}

uint64_t portableRandom64(portableRandomState *r) {
    uint64_t val;
    pid_t pid;
    bool new_pid;

    randomSetup(val);
    genWidth(val);

    return val;
}

/* Words generated per pid check and budget charge in
 * portableRandomFill64.  Far below the refuel budget, so a fill never
 * emits more than DK_PR_REFUEL_BYTES under one key. */
#define DK_PR_FILL_CHUNK_WORDS 256

void portableRandomFill64(portableRandomState *r, uint64_t *out, size_t count) {
    while (count > 0) {
        const size_t words =
            count < DK_PR_FILL_CHUNK_WORDS ? count : DK_PR_FILL_CHUNK_WORDS;
        const int64_t charge = (int64_t)(words * sizeof(*out));

        pid_t pid = r->pid;
        bool newPid = false;
        if (pid != -1) {
            pid = getpid();
            newPid = pid != r->pid;
        }

        /* Re-key before generating if this chunk would exhaust the
         * budget, then charge the chunk to the new key. */
        r->remainingBytesBeforeRefuel -= charge;
        if (r->remainingBytesBeforeRefuel <= 0 || newPid) {
            portableRandom_stir(r, pid);
            r->remainingBytesBeforeRefuel -= charge;
        }

        for (size_t w = 0; w < words; w++) {
            uint64_t val;
            genWidth(val);
            out[w] = val;
        }

        out += words;
        count -= words;
    }
}

#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"

#include <sys/wait.h>

defcleanerName(uint8_t *, zfree, Bytes);

/* Draw one word through portableRandom64, or through a one-word
 * portableRandomFill64 if 'fill'. */
static uint64_t portableRandomTestDraw(portableRandomState *r,
                                       const bool fill) {
    if (fill) {
        uint64_t word = 0;
        portableRandomFill64(r, &word, 1);
        return word;
    }

    return portableRandom64(r);
}

/* Fork; draw one word from 'r' in the child (sent back over a pipe) and
 * one in the parent, through portableRandomFill64 if 'fill'.  Returns
 * false if pipe/fork/read failed.  The child _exits so it runs no atexit
 * or sanitizer handlers. */
static bool portableRandomTestForkedNextWord(portableRandomState *r,
                                             const bool fill,
                                             uint64_t *childWord,
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
        const uint64_t word = portableRandomTestDraw(r, fill);
        const ssize_t wrote = write(fds[1], &word, sizeof(word));
        (void)close(fds[1]);
        _exit(wrote == (ssize_t)sizeof(word) ? 0 : 1);
    }

    (void)close(fds[1]);
    *parentWord = portableRandomTestDraw(r, fill);

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

static bool portableRandomTestAllZero(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i]) {
            return false;
        }
    }

    return true;
}

/* Copy 'r' into 'frozen' with its fork check off and a budget so large
 * that it never re-keys: 'frozen' then continues r's current key
 * stream, which a re-keyed 'r' must leave. */
static void portableRandomTestFreeze(const portableRandomState *r,
                                     portableRandomState *frozen) {
    *frozen = *r;
    frozen->pid = -1;
    frozen->remainingBytesBeforeRefuel = INT64_MAX / 2;
}

int portableRandomTest(int, char *[]) {
    int err = 0;

    TEST("the refuel budget is charged one word width per word") {
        /* portableRandom64 charges sizeof(uint64_t) of refuel budget per
         * call.  The first draw after init stirs and resets the budget
         * to DK_PR_REFUEL_BYTES. */
        portableRandomState r;
        portableRandom_init(&r, getpid());

        (void)portableRandom64(&r); /* crosses the initial refuel edge */

        if (r.remainingBytesBeforeRefuel != (int64_t)DK_PR_REFUEL_BYTES) {
            ERR("after the first draw the budget is %" PRId64 ", not the "
                "refuel budget %" PRIu64,
                (int64_t)r.remainingBytesBeforeRefuel,
                (uint64_t)DK_PR_REFUEL_BYTES);
        }

        const uint64_t wordBytes = (uint64_t)sizeof(uint64_t);
        const uint64_t draws = 1000; /* far below one refuel interval */

        for (uint64_t d = 0; d < draws; d++) {
            (void)portableRandom64(&r);
        }

        const int64_t want =
            (int64_t)DK_PR_REFUEL_BYTES - (int64_t)(draws * wordBytes);
        if (r.remainingBytesBeforeRefuel != want) {
            ERR("after %" PRIu64 " draws of %" PRIu64 " bytes the budget is "
                "%" PRId64 ", not %" PRId64,
                draws, wordBytes, (int64_t)r.remainingBytesBeforeRefuel, want);
        }
    }

    TEST("the refuel edge is crossed exactly once, at the budget") {
        /* The re-stir happens on exactly the draw that exhausts the
         * budget. */
        portableRandomState r;
        portableRandom_init(&r, getpid());

        (void)portableRandom64(&r); /* budget := DK_PR_REFUEL_BYTES */

        const uint64_t wordBytes = (uint64_t)sizeof(uint64_t);
        const uint64_t wordsPerRefuel =
            (uint64_t)DK_PR_REFUEL_BYTES / wordBytes;

        /* Stop one draw short of the edge. */
        for (uint64_t d = 1; d < wordsPerRefuel; d++) {
            (void)portableRandom64(&r);
        }

        const int64_t beforeEdge = (int64_t)r.remainingBytesBeforeRefuel;
        if (beforeEdge != (int64_t)wordBytes) {
            ERR("one draw short of the refuel edge the budget is %" PRId64
                ", not one word (%" PRIu64 ")",
                beforeEdge, wordBytes);
        }

        (void)portableRandom64(&r); /* the draw that crosses */

        if (r.remainingBytesBeforeRefuel != (int64_t)DK_PR_REFUEL_BYTES) {
            ERR("crossing the refuel edge left the budget at %" PRId64
                ", not the full refuel budget %" PRIu64,
                (int64_t)r.remainingBytesBeforeRefuel,
                (uint64_t)DK_PR_REFUEL_BYTES);
        }
    }

    TEST("portableRandom32 charges four bytes per word") {
        /* The first draw after init stirs and installs the full budget;
         * each later draw charges sizeof(uint32_t), and the draw that
         * exhausts the budget re-stirs. */
        portableRandomState r;
        portableRandom_init(&r, getpid());
        (void)portableRandom32(&r);

        if (r.remainingBytesBeforeRefuel != (int64_t)DK_PR_REFUEL_BYTES) {
            ERR("after the first draw the budget is %" PRId64 ", not "
                "%" PRIu64,
                (int64_t)r.remainingBytesBeforeRefuel,
                (uint64_t)DK_PR_REFUEL_BYTES);
        }

        const uint64_t wordBytes = (uint64_t)sizeof(uint32_t);
        const uint64_t wordsPerRefuel =
            (uint64_t)DK_PR_REFUEL_BYTES / wordBytes;
        for (uint64_t d = 1; d < wordsPerRefuel; d++) {
            (void)portableRandom32(&r);
        }

        if (r.remainingBytesBeforeRefuel != (int64_t)wordBytes) {
            ERR("one draw short of the refuel edge the budget is %" PRId64
                ", not one word (%" PRIu64 ")",
                (int64_t)r.remainingBytesBeforeRefuel, wordBytes);
        }

        (void)portableRandom32(&r);

        if (r.remainingBytesBeforeRefuel != (int64_t)DK_PR_REFUEL_BYTES) {
            ERR("crossing the refuel edge left the budget at %" PRId64
                ", not %" PRIu64,
                (int64_t)r.remainingBytesBeforeRefuel,
                (uint64_t)DK_PR_REFUEL_BYTES);
        }
    }

    TEST("crossing the refuel edge re-keys the stream") {
        /* A frozen copy continues the current key stream.  Before the
         * edge the state matches it word for word; the draw that crosses
         * the edge re-keys, so it must differ (false-positive 2^-64). */
        const int64_t wordBytes = (int64_t)sizeof(uint64_t);
        portableRandomState r;
        portableRandom_init(&r, getpid());
        (void)portableRandom64(&r);
        r.remainingBytesBeforeRefuel = 2 * wordBytes;

        portableRandomState frozen;
        portableRandomTestFreeze(&r, &frozen);

        const uint64_t before = portableRandom64(&r);
        if (before != portableRandom64(&frozen)) {
            ERR("a draw before the refuel edge left the frozen stream");
        }

        const uint64_t crossing = portableRandom64(&r);
        if (crossing == portableRandom64(&frozen)) {
            ERR("the draw that crossed the refuel edge returned 0x%016" PRIx64
                ", the old key's next word",
                crossing);
        }
    }

    TEST("portableRandomFill64 returns portableRandom64's words") {
        /* Two frozen copies of one state: a 300-word fill (two chunks)
         * from one must equal 300 portableRandom64 draws from the
         * other. */
        portableRandomState r;
        portableRandom_init(&r, getpid());
        (void)portableRandom64(&r);

        portableRandomState byFill;
        portableRandomState byWord;
        portableRandomTestFreeze(&r, &byFill);
        portableRandomTestFreeze(&r, &byWord);

        uint64_t filled[300];
        portableRandomFill64(&byFill, filled, COUNT_ARRAY(filled));

        for (size_t w = 0; w < COUNT_ARRAY(filled); w++) {
            const uint64_t want = portableRandom64(&byWord);
            if (filled[w] != want) {
                ERR("fill word %zu is 0x%016" PRIx64 ", not 0x%016" PRIx64, w,
                    filled[w], want);
                break;
            }
        }
    }

    TEST("portableRandomFill64 charges eight bytes per word") {
        /* A 64-word fill charges 512 bytes and a 300-word fill 2400,
         * whatever the chunking. */
        portableRandomState r;
        portableRandom_init(&r, getpid());
        (void)portableRandom64(&r); /* budget := DK_PR_REFUEL_BYTES */

        uint64_t out[300];
        portableRandomFill64(&r, out, 64);
        int64_t want = (int64_t)DK_PR_REFUEL_BYTES - 64 * 8;
        if (r.remainingBytesBeforeRefuel != want) {
            ERR("after a 64-word fill the budget is %" PRId64 ", not "
                "%" PRId64,
                (int64_t)r.remainingBytesBeforeRefuel, want);
        }

        portableRandomFill64(&r, out, COUNT_ARRAY(out));
        want -= (int64_t)(COUNT_ARRAY(out) * 8);
        if (r.remainingBytesBeforeRefuel != want) {
            ERR("after a 300-word fill the budget is %" PRId64 ", not "
                "%" PRId64,
                (int64_t)r.remainingBytesBeforeRefuel, want);
        }

        /* A zero-word fill charges nothing and writes nothing. */
        out[0] = 0x5A;
        portableRandomFill64(&r, out, 0);
        if (r.remainingBytesBeforeRefuel != want || out[0] != 0x5A) {
            ERR("a zero-word fill changed the budget or the output");
        }
    }

    TEST("portableRandomFill64 re-keys before a chunk that exhausts the "
         "budget") {
        /* With room for the chunk plus one word, the fill stays on the
         * old key (it matches a frozen copy).  With room for one word
         * less than the chunk, it re-keys first: its first word leaves
         * the frozen stream (false-positive 2^-64) and the new budget is
         * charged for the whole chunk. */
        const int64_t chunkBytes = 64 * (int64_t)sizeof(uint64_t);
        const int64_t slack[] = {(int64_t)sizeof(uint64_t),
                                 -(int64_t)sizeof(uint64_t)};

        for (size_t c = 0; c < COUNT_ARRAY(slack); c++) {
            const bool rekeys = slack[c] < 0;
            portableRandomState r;
            portableRandom_init(&r, getpid());
            (void)portableRandom64(&r);
            r.remainingBytesBeforeRefuel = chunkBytes + slack[c];

            portableRandomState frozen;
            portableRandomTestFreeze(&r, &frozen);

            uint64_t got[64];
            uint64_t old[64];
            portableRandomFill64(&r, got, COUNT_ARRAY(got));
            portableRandomFill64(&frozen, old, COUNT_ARRAY(old));

            const int64_t want =
                rekeys ? (int64_t)DK_PR_REFUEL_BYTES - chunkBytes : slack[c];
            if (r.remainingBytesBeforeRefuel != want) {
                ERR("slack %" PRId64 ": the budget is %" PRId64 ", not "
                    "%" PRId64,
                    slack[c], (int64_t)r.remainingBytesBeforeRefuel, want);
            }

            if (rekeys && got[0] == old[0]) {
                ERR("a fill that exhausts the budget generated its first "
                    "word from the old key");
            }

            if (!rekeys && memcmp(got, old, sizeof(got)) != 0) {
                ERR("a fill within the budget left the old key stream");
            }
        }
    }

    TEST("a fill larger than the budget re-keys within the fill") {
        /* Two and a bit refuel intervals of words in one call.  The fill
         * must re-key as it goes, so the budget ends positive and no key
         * emitted more than DK_PR_REFUEL_BYTES. */
        const size_t words =
            2 * ((size_t)DK_PR_REFUEL_BYTES / sizeof(uint64_t)) + 5;
        cleanerName(zfree, Bytes) uint8_t *buf =
            zcalloc(words, sizeof(uint64_t));

        portableRandomState r;
        portableRandom_init(&r, getpid());
        portableRandomFill64(&r, (uint64_t *)buf, words);

        if (r.remainingBytesBeforeRefuel <= 0 ||
            r.remainingBytesBeforeRefuel > (int64_t)DK_PR_REFUEL_BYTES) {
            ERR("after a %zu-word fill the budget is %" PRId64
                ", outside (0, %" PRIu64 "]",
                words, (int64_t)r.remainingBytesBeforeRefuel,
                (uint64_t)DK_PR_REFUEL_BYTES);
        }
    }

    TEST("an empty key leaves the state unchanged") {
        /* portableRandom_add with len 0 must return without mixing (and
         * without dividing by zero). */
        portableRandomState r;
        portableRandom_init(&r, getpid());
        (void)portableRandom64(&r);

        uint8_t s[sizeof(r.s)];
        memcpy(s, r.s, sizeof(s));
        const uint8_t i = r.i;
        const uint8_t j = r.j;

        const uint8_t key[1] = {0x5A};
        portableRandom_add(&r, key, 0);

        if (r.i != i || r.j != j || memcmp(s, r.s, sizeof(s)) != 0) {
            ERR("portableRandom_add with an empty key changed the state");
        } else {
            /* A caller with no key may well pass NULL. */
            portableRandom_add(&r, NULL, 0);

            if (r.i != i || r.j != j || memcmp(s, r.s, sizeof(s)) != 0) {
                ERR("portableRandom_add(NULL, 0) changed the state");
            }
        }
    }

    TEST("the /dev/urandom reader fills every byte it is asked for") {
        /* The reader backs portableRandomDirect when no OS call was
         * detected, and every stir whose OS call fails.  A 300-byte read
         * must return 300, leave the guard untouched, not be all zero,
         * and differ from a second read (false-positive 2^-2400).  An
         * empty read returns 0. */
        uint8_t a[300 + 16] = {};
        uint8_t b[300 + 16] = {};
        const size_t len = 300;

        if (portableRandomUrandom(a, len) != (ssize_t)len ||
            portableRandomUrandom(b, len) != (ssize_t)len) {
            ERR("a 300-byte /dev/urandom read came back short");
        } else if (!portableRandomTestAllZero(a + len, sizeof(a) - len) ||
                   !portableRandomTestAllZero(b + len, sizeof(b) - len)) {
            ERR("a /dev/urandom read wrote past the end");
        } else if (portableRandomTestAllZero(a, len) ||
                   memcmp(a, b, len) == 0) {
            ERR("two /dev/urandom reads were all zero or identical");
        }

        if (portableRandomUrandom(a, 0) != 0) {
            ERR("an empty /dev/urandom read must return 0");
        }
    }

    TEST("portableRandomDirectFull fills every byte it is asked for") {
        /* Each fill must succeed, write exactly the requested length
         * (the guard bytes after it stay untouched), not be all zero,
         * and differ from a second fill of the same length.  The
         * all-zero and repeat checks apply from 16 bytes up, where an
         * OS source fails them with probability at most 2^-128. */
        const size_t lens[] = {1, 256, 257, 4096};
        const size_t guard = 16;

        for (size_t i = 0; i < COUNT_ARRAY(lens); i++) {
            const size_t len = lens[i];
            cleanerName(zfree, Bytes) uint8_t *a = zcalloc(1, len + guard);
            cleanerName(zfree, Bytes) uint8_t *b = zcalloc(1, len + guard);

            if (!portableRandomDirectFull(a, len) ||
                !portableRandomDirectFull(b, len)) {
                ERR("portableRandomDirectFull(%zu) returned false", len);
            } else {
                if (!portableRandomTestAllZero(a + len, guard) ||
                    !portableRandomTestAllZero(b + len, guard)) {
                    ERR("portableRandomDirectFull(%zu) wrote past the end",
                        len);
                }

                if (len >= 16) {
                    if (portableRandomTestAllZero(a, len)) {
                        ERR("portableRandomDirectFull(%zu) left the buffer "
                            "all zero",
                            len);
                    }

                    if (memcmp(a, b, len) == 0) {
                        ERR("two portableRandomDirectFull(%zu) fills are "
                            "identical",
                            len);
                    }
                }
            }
        }

        /* One-byte fills: at least one of 32 must be nonzero
         * (false-positive 2^-256). */
        bool anyNonZero = false;
        for (uint32_t t = 0; t < 32 && !anyNonZero; t++) {
            uint8_t one = 0;
            if (!portableRandomDirectFull(&one, 1)) {
                ERR("portableRandomDirectFull(1) returned false");
                break;
            }

            anyNonZero = one != 0;
        }

        if (!anyNonZero) {
            ERR("32 one-byte portableRandomDirectFull fills were all zero");
        }

        uint8_t untouched = 0x5A;
        if (!portableRandomDirectFull(&untouched, 0) || untouched != 0x5A) {
            ERR("a zero-length portableRandomDirectFull must succeed "
                "without writing");
        }
    }

    TEST("a real pid makes a forked child's next word differ") {
        /* With a real pid, a forked child re-keys, so its next word
         * differs from the parent's (false-positive 2^-64), whether it
         * is drawn by portableRandom64 or by portableRandomFill64.  The
         * initial stir runs before the fork so neither side crosses a
         * refuel edge. */
        for (uint32_t c = 0; c < 2; c++) {
            const bool fill = c == 1;
            portableRandomState r;
            portableRandom_init(&r, getpid());
            (void)portableRandom64(&r);

            uint64_t childWord = 0;
            uint64_t parentWord = 0;
            if (!portableRandomTestForkedNextWord(&r, fill, &childWord,
                                                  &parentWord)) {
                ERR("the fork probe could not run (pipe/fork/read failed)");
            } else if (childWord == parentWord) {
                ERR("%s: a forked child drew 0x%016" PRIx64 ", the same "
                    "word as the parent",
                    fill ? "fill" : "word", childWord);
            }
        }
    }

    TEST("a -1 pid disables the fork re-key (negative control)") {
        /* Negative control: with pid -1 the fork check is disabled, so
         * the child's next word equals the parent's. */
        portableRandomState r;
        portableRandom_init(&r, -1);
        (void)portableRandom64(&r);

        uint64_t childWord = 0;
        uint64_t parentWord = 0;
        if (!portableRandomTestForkedNextWord(&r, false, &childWord,
                                              &parentWord)) {
            ERR("the fork probe could not run (pipe/fork/read failed)");
        } else if (childWord != parentWord) {
            ERR("with pid -1 a forked child drew 0x%016" PRIx64 " and the "
                "parent 0x%016" PRIx64,
                childWord, parentWord);
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
