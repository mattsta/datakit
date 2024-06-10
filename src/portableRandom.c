/* Originally from njs/nxt nginx BSD license */

#include "datakit.h"

#include <sys/types.h>
#include <unistd.h>

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

    /* Set indexes 0 to 255 to values 0 to 255 */
    for (size_t i = 0; i < sizeof(r->s); i++) {
        r->s[i] = i;
    }

    /* Attempt to overwrite the initial state with actual random values */
    portableRandomDirect(r->s, sizeof(r->s));
}

ssize_t portableRandomDirect(void *const dst, const size_t requestedLen) {
    /* "1" means GRND_NONBLOCK. If no entropy and WOULDBLOCK, -1 is returned
     * with EAGAIN. */
    const uint32_t DK_GRND_NONBLOCK = 1;

#if DK_HAVE_GETRANDOM
    return getrandom(dst, requestedLen, DK_GRND_NONBLOCK);
#elif DK_HAVE_LINUX_SYS_GETRANDOM
    /* Linux 3.17 SYS_getrandom, not available in Glibc prior to 2.25. */
    return syscall(SYS_getrandom, dst, requestedLen, DK_GRND_NONBLOCK);
#elif DK_HAVE_GETENTROPY || DK_HAVE_GETENTROPY_SYS_RANDOM
    (void)DK_GRND_NONBLOCK;
    return getentropy(dst, requestedLen);
#else
    /* Random source not found! */
    return -1;
#endif
}

void portableRandom_stir(portableRandomState *r, pid_t pid) {
    int fd;
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

#if DK_HAVE_GETRANDOM
    n = getrandom(&key.bytes, DK_PR_KEYSIZE, 0);
#elif DK_HAVE_LINUX_SYS_GETRANDOM
    /* Linux 3.17 SYS_getrandom, not available in Glibc prior to 2.25. */
    n = syscall(SYS_getrandom, &key.bytes, DK_PR_KEYSIZE, 0);
#elif DK_HAVE_GETENTROPY || DK_HAVE_GETENTROPY_SYS_RANDOM
    n = 0;
    if (getentropy(&key.bytes, DK_PR_KEYSIZE) == 0) {
        n = DK_PR_KEYSIZE;
    }
#else
    n = 0;
#endif

    if (n != DK_PR_KEYSIZE) {
        fd = open("/dev/urandom", O_RDONLY);

        if (fd >= 0) {
            n = read(fd, &key.bytes, DK_PR_KEYSIZE);
            (void)close(fd);
        }
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
        const uint8_t dropped = portableRandom_byte(r);
        (void)dropped;
    }

    /* Seed with system entropy again after 100k bytes consumed */
    r->remainingBytesBeforeRefuel = 100000;
}

void portableRandom_add(portableRandomState *r, const uint8_t *key,
                        size_t len) {
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
            val |= (__typeof(val))portableRandom_byte(r)                       \
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

void portableRandomBy8(portableRandomState *r, void *const fill_,
                       const size_t count) {
    uint64_t *fill = fill_;
    pid_t pid;
    bool new_pid;

    randomSetup(*fill);
    for (size_t f = 0; f < count; f++) {
        genWidth(fill[f]);
    }
}

void portableRandomBy16(portableRandomState *r, void *const fill_,
                        const size_t count) {
    __uint128_t *fill = fill_;
    pid_t pid;
    bool new_pid;

    randomSetup(*fill);
    for (size_t f = 0; f < count; f++) {
        genWidth(fill[f]);
    }
}
