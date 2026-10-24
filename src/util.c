#include "util.h"
#include "str.h"

#include <stdio.h>
#include <strings.h> /* strncasecmp */

/* ====================================================================
 * Readable string-to-value helpers
 * ==================================================================== */
/* Every unit humanToBytes accepts. Z and Y units are absent on purpose:
 * 1 ZB (10^21) and 1 ZiB (2^70) already exceed UINT64_MAX, so they are
 * refused like any other unknown unit. */
static const struct {
    const char *name;
    uint64_t mul;
} humanUnits[] = {
    {"b", 1ULL},
    {"k", 1000ULL},
    {"kb", 1000ULL},
    {"kib", 1ULL << 10},
    {"m", 1000ULL * 1000},
    {"mb", 1000ULL * 1000},
    {"mib", 1ULL << 20},
    {"g", 1000ULL * 1000 * 1000},
    {"gb", 1000ULL * 1000 * 1000},
    {"gib", 1ULL << 30},
    {"t", 1000ULL * 1000 * 1000 * 1000},
    {"tb", 1000ULL * 1000 * 1000 * 1000},
    {"tib", 1ULL << 40},
    {"p", 1000ULL * 1000 * 1000 * 1000 * 1000},
    {"pb", 1000ULL * 1000 * 1000 * 1000 * 1000},
    {"pib", 1ULL << 50},
    {"e", 1000ULL * 1000 * 1000 * 1000 * 1000 * 1000},
    {"eb", 1000ULL * 1000 * 1000 * 1000 * 1000 * 1000},
    {"eib", 1ULL << 60},
};

static uint64_t humanToBytesRefuse(bool *err) {
    if (err) {
        *err = true;
    }

    return 0;
}

uint64_t humanToBytes(const void *buf_, const size_t len, bool *err) {
    const char *buf = buf_;

    if (err) {
        *err = false;
    }

    /* Digits come first */
    size_t digitsCount = 0;
    while (digitsCount < len && StrIsdigit(buf[digitsCount])) {
        digitsCount++;
    }

    uint64_t val;
    if (!StrBufToUInt64(buf, digitsCount, &val)) {
        return humanToBytesRefuse(err);
    }

    /* Then optional white space, then the unit: the rest of 'buf', up to
     * 'len' or a NUL, whichever comes first. */
    size_t at = digitsCount;
    while (at < len && StrIsspace(buf[at])) {
        at++;
    }

    const char *const unit = buf + at;
    const size_t unitLen = strnlen(unit, len - at);

    uint64_t mul = 0;
    if (unitLen == 0) {
        mul = 1;
    } else {
        for (size_t i = 0; i < COUNT_ARRAY(humanUnits); i++) {
            if (strlen(humanUnits[i].name) == unitLen &&
                !strncasecmp(unit, humanUnits[i].name, unitLen)) {
                mul = humanUnits[i].mul;
                break;
            }
        }
    }

    /* Unknown unit, or a product that does not fit */
    if (mul == 0 || val > UINT64_MAX / mul) {
        return humanToBytesRefuse(err);
    }

    return val * mul;
}

/* ====================================================================
 * Readable value-to-string helpers
 * ==================================================================== */
void secondsToHuman(void *const buf, const size_t len, const double sec) {
    if (sec < 0.000001) {
        snprintf(buf, len, "%.4f ns", sec * 1000000000);
    } else if (sec < 0.001) {
        snprintf(buf, len, "%.4f us", sec * 1000000);
    } else if (sec < 1) {
        snprintf(buf, len, "%.4f ms", sec * 1000);
    } else if (sec < 60) {
        snprintf(buf, len, "%.4f seconds", sec);
    } else if (sec < 3600) {
        snprintf(buf, len, "%.4f minutes", sec / 60);
    } else if (sec < 86400) {
        snprintf(buf, len, "%.4f hours", sec / 60 / 60);
    } else {
        snprintf(buf, len, "%.4f days", sec / 60 / 60 / 24);
    }
}

bool bytesToHuman(void *const buf, const size_t len, const uint64_t n) {
    double d;

    if (n < 1024) {
        snprintf(buf, len, "%" PRIu64 " B", n);
    } else if (n < (1024ULL * 1024)) {
        d = (double)n / (1024);
        snprintf(buf, len, "%.5f KiB", d);
    } else if (n < (1024ULL * 1024 * 1024)) {
        d = (double)n / (1024 * 1024);
        snprintf(buf, len, "%.5f MiB", d);
    } else if (n < (1024ULL * 1024 * 1024 * 1024)) {
        d = (double)n / (1024LL * 1024 * 1024);
        snprintf(buf, len, "%.5f GiB", d);
    } else if (n < (1024ULL * 1024 * 1024 * 1024 * 1024)) {
        d = (double)n / (1024LL * 1024 * 1024 * 1024);
        snprintf(buf, len, "%.5f TiB", d);
    } else if (n < (1024ULL * 1024 * 1024 * 1024 * 1024 * 1024)) {
        d = (double)n / (1024LL * 1024 * 1024 * 1024 * 1024);
        snprintf(buf, len, "%.5f PiB", d);
    } else {
        /* UINT64_MAX is just under 16 EiB, so EiB is the largest unit any
         * uint64_t needs. */
        d = (double)n / (1024ULL * 1024 * 1024 * 1024 * 1024 * 1024);
        snprintf(buf, len, "%.5f EiB", d);
    }

    return true;
}

void getRandomHexChars(uint8_t *p, uint32_t len) {
    FILE *fp = fopen("/dev/urandom", "r");
    static const char *charset = "0123456789abcdef";

    if (fp == NULL || fread(p, len, 1, fp) == 0) {
        /* If we can't read from /dev/urandom because of permission problems,
         * attempt to generate entropy given our userspace access. */
        uint8_t *x = p;
        uint32_t l = len;
        const pid_t pid = getpid();

        /* Use time and PID to fill the initial array. */
        struct timeval tv;
        gettimeofday(&tv, NULL);

        if (l >= sizeof(tv.tv_usec)) {
            memcpy(x, &tv.tv_usec, sizeof(tv.tv_usec));
            l -= sizeof(tv.tv_usec);
            x += sizeof(tv.tv_usec);
        }

        if (l >= sizeof(tv.tv_sec)) {
            memcpy(x, &tv.tv_sec, sizeof(tv.tv_sec));
            l -= sizeof(tv.tv_sec);
            x += sizeof(tv.tv_sec);
        }

        if (l >= sizeof(pid)) {
            memcpy(x, &pid, sizeof(pid));
        }

        /* Finally xor it with rand() output, that was already seeded with
         * time() at startup. */
        for (uint32_t j = 0; j < len; j++) {
            p[j] ^= rand();
        }
    }

    /* Turn it into hex digits taking just 4 bits out of 8 for every byte. */
    for (uint32_t j = 0; j < len; j++) {
        p[j] = charset[p[j] & 0x0F];
    }

    if (fp) {
        fclose(fp);
    }
}

bool getRandomHexCharsCounterInit(uint8_t seed[], uint8_t seedLen) {
    if (seedLen != SHA1_DIGEST_LENGTH) {
        return false;
    }

    /* Initialize a seed and use SHA1 in counter mode, where we hash
     * the same seed with a progressive counter. For the goals of this
     * function we just need non-colliding strings, there are no
     * cryptographic security needs. */
    FILE *fp = fopen("/dev/urandom", "r");
    if (!fp) {
        return false;
    }

    bool readSuccess = true;
    if (fread(seed, seedLen, 1, fp) != 1) {
        readSuccess = false;
    }

    fclose(fp);

    return readSuccess;
}

bool getRandomHexCharsCounter(uint8_t seed[SHA1_DIGEST_LENGTH],
                              uint64_t *counter, uint8_t *p, uint32_t len) {
    static const char *charset = "0123456789abcdef";

    if (!seed || !counter || !p) {
        return false;
    }

    while (len) {
        SHA1_CTX ctx = {{0}};
        uint8_t digest[SHA1_DIGEST_LENGTH] = {0};
        uint32_t copylen = len > SHA1_DIGEST_LENGTH ? SHA1_DIGEST_LENGTH : len;

        SHA1Init(&ctx);
        SHA1Update(&ctx, seed, SHA1_DIGEST_LENGTH);
        SHA1Update(&ctx, (uint8_t *)counter, sizeof(*counter));
        SHA1Final(digest, &ctx);
        (*counter)++;

        memcpy(p, digest, copylen);

        /* Convert to hex digits. */
        for (uint32_t j = 0; j < copylen; j++) {
            p[j] = charset[p[j] & 0x0F];
        }

        len -= copylen;
        p += copylen;
    }

    return true;
}

/* Return true if the specified path is just a file basename without any
 * relative or absolute path. This function just checks that no / or \
 * character exists inside the specified path */
bool pathIsBaseName(char *path) {
    return strchr(path, '/') == NULL && strchr(path, '\\') == NULL;
}

/* ====================================================================
 * Weird pattern-vs-string match with limited features
 * ==================================================================== */
static bool globByteEq(uint8_t a, uint8_t b, int nocase) {
    return nocase ? StrTolower(a) == StrTolower(b) : a == b;
}

/* The nesting of stars past which a pattern is refused (each costs a stack
 * frame) */
#define GLOB_MAX_NESTING 1000

/* One step of the match.  Every element but '*' consumes exactly one byte
 * of the string, so it needs one; every read is inside its length.  A star
 * tries each split of the rest of the string, but once a nested star has
 * run out of string ('*skipLonger'), a later start for this star cannot
 * match either (the elements between stars are fixed-width), so the search
 * stops: polynomial instead of exponential in the number of stars. */
static bool globMatch(const uint8_t *pattern, int patternLen,
                      const uint8_t *string, int stringLen, int nocase,
                      bool *skipLonger, int nesting) {
    if (nesting > GLOB_MAX_NESTING) {
        return false;
    }

    while (patternLen > 0) {
        if (pattern[0] == '*') {
            while (patternLen > 1 && pattern[1] == '*') {
                pattern++;
                patternLen--;
            }
            if (patternLen == 1) {
                return true; /* a trailing star matches the rest */
            }
            for (; stringLen > 0; string++, stringLen--) {
                if (globMatch(pattern + 1, patternLen - 1, string, stringLen,
                              nocase, skipLonger, nesting + 1)) {
                    return true;
                }
                if (*skipLonger) {
                    return false;
                }
            }
            *skipLonger = true;
            return false;
        }

        if (stringLen <= 0) {
            return false;
        }
        const uint8_t c = string[0];

        if (pattern[0] == '?') {
            pattern++;
            patternLen--;
        } else if (pattern[0] == '[') {
            pattern++;
            patternLen--;
            const bool not = patternLen > 0 && pattern[0] == '^';
            if (not) {
                pattern++;
                patternLen--;
            }
            /* To the closing ']', or the pattern's end for an unterminated
             * class.  A backslash escapes the next byte; a trailing one is
             * itself. */
            bool match = false;
            while (patternLen > 0 && pattern[0] != ']') {
                if (pattern[0] == '\\' && patternLen >= 2) {
                    match |= globByteEq(pattern[1], c, nocase);
                    pattern += 2;
                    patternLen -= 2;
                } else if (patternLen >= 3 && pattern[1] == '-') {
                    int start = pattern[0];
                    int end = pattern[2];
                    int x = c;
                    if (start > end) {
                        const int t = start;
                        start = end;
                        end = t;
                    }
                    if (nocase) {
                        start = StrTolower(start);
                        end = StrTolower(end);
                        x = StrTolower(x);
                    }
                    match |= x >= start && x <= end;
                    pattern += 3;
                    patternLen -= 3;
                } else {
                    match |= globByteEq(pattern[0], c, nocase);
                    pattern++;
                    patternLen--;
                }
            }
            if (patternLen > 0) {
                pattern++; /* the ']' */
                patternLen--;
            }
            if (match == not) {
                return false;
            }
        } else {
            if (pattern[0] == '\\' && patternLen >= 2) {
                pattern++;
                patternLen--;
            }
            if (!globByteEq(pattern[0], c, nocase)) {
                return false;
            }
            pattern++;
            patternLen--;
        }
        string++;
        stringLen--;
    }

    return stringLen <= 0;
}

/* Glob-style pattern matching over 'patternLen' and 'stringLen' bytes (no
 * NUL needed): '*' any run, '?' one byte, '[...]' a class ('^' negates,
 * 'a-z' ranges, '\' escapes), '\' escapes outside one too.  A pattern
 * nesting more than GLOB_MAX_NESTING stars matches nothing. */
bool stringmatchlen(const char *pattern, int patternLen, const char *string,
                    int stringLen, int nocase) {
    bool skipLonger = false;
    return globMatch((const uint8_t *)pattern, patternLen,
                     (const uint8_t *)string, stringLen, nocase, &skipLonger,
                     0);
}

bool stringmatch(const char *pattern, const char *string, int nocase) {
    return stringmatchlen(pattern, strlen(pattern), string, strlen(string),
                          nocase);
}

/* ====================================================================
 * Instruction Womps
 * ==================================================================== */
/* Scans x86 code starting at addr, for a max of `len`
 * bytes, searching for E8 (callq) opcodes, and dumping the symbols
 * and the call offset if they appear to be valid. */
#if 0
#include <dlfcn.h>
void dumpX86Calls(void *addr, size_t len) {
    size_t j;
    unsigned char *p = addr;
    Dl_info info;
    /* Hash table to best-effort avoid printing the same symbol
     * multiple times. */
    uintptr_t ht[256] = {0};

    if (len < 5) {
        return;
    }

    for (j = 0; j < len - 4; j++) {
        if (p[j] != 0xE8) {
            continue; /* Not an E8 CALL opcode. */
        }

        uintptr_t target = (uintptr_t)addr + j + 5;
        target += *((int32_t *)(p + j + 1));
        if (dladdr((void *)target, &info) != 0 && info.dli_sname != NULL) {
            if (ht[target & 0xff] != target) {
                printf("Function at 0x%" PRIxPTR " is %s\n", target, info.dli_sname);
                ht[target & 0xff] = target;
            }

            j += 4; /* Skip the 32 bit immediate. */
        }
    }
}
#endif

#ifdef DATAKIT_TEST
#include "asmUtils.h"
#include "cleaner.h"
#include "ctest.h"
#include <assert.h>

defcleanerName(uint8_t *, zfree, Bytes);

/* An independent glob oracle: the pattern parsed into elements (a star, or
 * the set of bytes one position accepts), then matched by dynamic
 * programming over (element, position), with no recursion and no early
 * stop.  Small inputs only. */
#define GLOB_ORACLE_MAX 16
typedef struct globOracleElem {
    bool star;
    uint8_t accept[32]; /* bit per byte */
} globOracleElem;

static void globOracleAdd(globOracleElem *e, int b, int nocase) {
    e->accept[(uint8_t)b >> 3] |= (uint8_t)(1u << ((uint8_t)b & 7));
    if (nocase) {
        const int lo = StrTolower(b);
        const int up = (lo >= 'a' && lo <= 'z') ? lo - 'a' + 'A' : lo;
        e->accept[(uint8_t)lo >> 3] |= (uint8_t)(1u << ((uint8_t)lo & 7));
        e->accept[(uint8_t)up >> 3] |= (uint8_t)(1u << ((uint8_t)up & 7));
    }
}

static bool globOracle(const char *p, int pl, const char *s, int sl,
                       int nocase) {
    globOracleElem el[GLOB_ORACLE_MAX];
    int n = 0;
    for (int i = 0; i < pl;) {
        globOracleElem *e = &el[n++];
        memset(e, 0, sizeof(*e));
        if (p[i] == '*') {
            e->star = true;
            i++;
        } else if (p[i] == '?') {
            memset(e->accept, 0xff, sizeof(e->accept));
            i++;
        } else if (p[i] == '[') {
            i++;
            const bool not = i < pl && p[i] == '^';
            i += not;
            while (i < pl && p[i] != ']') {
                if (p[i] == '\\' && i + 1 < pl) {
                    globOracleAdd(e, p[i + 1], nocase);
                    i += 2;
                } else if (i + 2 < pl && p[i + 1] == '-') {
                    int a = (uint8_t)p[i];
                    int b = (uint8_t)p[i + 2];
                    if (a > b) {
                        const int t = a;
                        a = b;
                        b = t;
                    }
                    for (int c = 0; c < 256; c++) {
                        const int x = nocase ? StrTolower(c) : c;
                        const int lo = nocase ? StrTolower(a) : a;
                        const int hi = nocase ? StrTolower(b) : b;
                        if (x >= lo && x <= hi) {
                            e->accept[c >> 3] |= (uint8_t)(1u << (c & 7));
                        }
                    }
                    i += 3;
                } else {
                    globOracleAdd(e, p[i], nocase);
                    i++;
                }
            }
            i += i < pl; /* the ']' */
            if (not) {
                for (size_t k = 0; k < sizeof(e->accept); k++) {
                    e->accept[k] = (uint8_t)~e->accept[k];
                }
            }
        } else {
            if (p[i] == '\\' && i + 1 < pl) {
                i++;
            }
            globOracleAdd(e, p[i], nocase);
            i++;
        }
    }

    /* ok[i][j]: elements i.. match string bytes j.. */
    bool ok[GLOB_ORACLE_MAX + 1][GLOB_ORACLE_MAX + 1];
    for (int i = n; i >= 0; i--) {
        for (int j = sl; j >= 0; j--) {
            if (i == n) {
                ok[i][j] = j == sl;
            } else if (el[i].star) {
                ok[i][j] = ok[i + 1][j] || (j < sl && ok[i][j + 1]);
            } else {
                const uint8_t c = (uint8_t)s[j < sl ? j : 0];
                ok[i][j] = j < sl && (el[i].accept[c >> 3] & (1u << (c & 7))) &&
                           ok[i + 1][j + 1];
            }
        }
    }
    return ok[0][0];
}

/* Reference implementation of pow2Ceiling64 for testing */
static uint64_t pow2Ceiling64_reference(uint64_t x) {
    if (x <= 1) {
        return 2;
    }

    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    x++;
    return x;
}

int utilTest(int argc, char **argv) {
    (void)argc;
    (void)argv;

    int err = 0;

    TEST("glob matching, human seconds, base names, counter hex") {
        static const struct {
            const char *pattern;
            const char *string;
            int nocase;
            bool match;
        } globs[] = {{"h*o", "hello", 0, true},
                     {"h?llo", "hello", 0, true},
                     {"h[ae]llo", "hallo", 0, true},
                     {"h[^e]llo", "hello", 0, false},
                     {"HELLO", "hello", 1, true},
                     {"HELLO", "hello", 0, false},
                     {"*", "", 0, true},
                     {"a\\*b", "a*b", 0, true}};
        for (size_t i = 0; i < sizeof(globs) / sizeof(*globs); i++) {
            if (stringmatch(globs[i].pattern, globs[i].string,
                            globs[i].nocase) != globs[i].match) {
                ERR("stringmatch(\"%s\", \"%s\")", globs[i].pattern,
                    globs[i].string);
            }
        }
        /* The length form reads only the given bytes */
        if (!stringmatchlen("ab*", 3, "abcdef", 3, 0) ||
            stringmatchlen("abc", 3, "abd", 3, 0)) {
            ERRR("stringmatchlen ignored its lengths");
        }

        /* Pattern and string each copied into an allocation of exactly
         * their length: the address sanitizer reports any read past
         * either (an empty string, a pattern ending inside a class, an
         * escape or a range) */
        static const struct {
            const char *pattern;
            const char *string;
            int nocase;
            bool match;
        } edges[] = {
            {"a", "", 0, false},      {"[a]", "", 0, false},
            {"\\a", "", 0, false},    {"?", "", 0, false},
            {"*", "", 0, true},       {"", "", 0, true},
            {"", "a", 0, false},      {"a**", "a", 0, true},
            {"a*b", "a", 0, false},   {"ab", "a", 0, false},
            {"[a", "a", 0, true},     {"[a\\", "\\", 0, true},
            {"[a-", "-", 0, true},    {"[a-", "b", 0, false},
            {"[^", "x", 0, true},     {"\\", "\\", 0, true},
            {"[c-a]", "b", 0, true},  {"[a-c]", "B", 1, true},
            {"[\\A]", "a", 1, true},  {"[]", "a", 0, false},
            {"[^]", "a", 0, true},    {"*.c", "x.c", 0, true},
            {"*.c", "x.h", 0, false}, {"a?c", "abc", 0, true},
        };
        for (size_t i = 0; i < sizeof(edges) / sizeof(*edges); i++) {
            const size_t pl = strlen(edges[i].pattern);
            const size_t sl = strlen(edges[i].string);
            cleanerName(zfree, Bytes) uint8_t *p = zmalloc(pl ? pl : 1);
            cleanerName(zfree, Bytes) uint8_t *s = zmalloc(sl ? sl : 1);
            memcpy(p, edges[i].pattern, pl);
            memcpy(s, edges[i].string, sl);
            if (stringmatchlen((const char *)p, (int)pl, (const char *)s,
                               (int)sl, edges[i].nocase) != edges[i].match) {
                ERR("stringmatchlen(\"%s\", \"%s\") wrong", edges[i].pattern,
                    edges[i].string);
            }
        }

        /* Random small patterns and strings against the oracle */
        {
            static const char palpha[] = "ab*?[]^-\\A";
            static const char salpha[] = "abAB-]^\\";
            uint64_t rng = 0x61b;
            for (int iter = 0; iter < 20000; iter++) {
                char p[GLOB_ORACLE_MAX];
                char s[GLOB_ORACLE_MAX];
                rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
                const int pl = (int)((rng >> 33) % 9);
                const int sl = (int)((rng >> 45) % 9);
                const int nocase = (int)((rng >> 60) & 1);
                for (int i = 0; i < pl; i++) {
                    rng = rng * 6364136223846793005ULL + 1;
                    p[i] = palpha[(rng >> 40) % (sizeof(palpha) - 1)];
                }
                for (int i = 0; i < sl; i++) {
                    rng = rng * 6364136223846793005ULL + 1;
                    s[i] = salpha[(rng >> 40) % (sizeof(salpha) - 1)];
                }
                if (stringmatchlen(p, pl, s, sl, nocase) !=
                    globOracle(p, pl, s, sl, nocase)) {
                    ERR("pattern \"%.*s\" string \"%.*s\" nocase %d: "
                        "disagrees with the oracle",
                        pl, p, sl, s, nocase);
                    break;
                }
            }
        }

        /* Stars that cannot all match fail in polynomial time, not by
         * trying every way to split the string among them */
        static const char hard[] = "*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*b";
        char run[200];
        memset(run, 'a', sizeof(run));
        if (stringmatchlen(hard, (int)strlen(hard), run, (int)sizeof(run), 0)) {
            ERRR("an unmatchable star pattern matched");
        }

        char human[64];
        static const struct {
            double sec;
            const char *unit;
        } spans[] = {{1e-8, " ns"},    {1e-5, " us"},     {0.5, " ms"},
                     {5, " seconds"},  {120, " minutes"}, {7200, " hours"},
                     {200000, " days"}};
        for (size_t i = 0; i < sizeof(spans) / sizeof(*spans); i++) {
            secondsToHuman(human, sizeof(human), spans[i].sec);
            if (!strstr(human, spans[i].unit)) {
                ERR("secondsToHuman(%g) = \"%s\"", spans[i].sec, human);
            }
        }

        char base[] = "file.txt";
        char unix[] = "dir/file.txt";
        char dos[] = "dir\\file.txt";
        if (!pathIsBaseName(base) || pathIsBaseName(unix) ||
            pathIsBaseName(dos)) {
            ERRR("pathIsBaseName misjudged a path");
        }

        /* Counter mode: the seed and counter decide the output */
        uint8_t seed[SHA1_DIGEST_LENGTH];
        uint8_t shortSeed[4];
        if (getRandomHexCharsCounterInit(shortSeed, sizeof(shortSeed)) ||
            !getRandomHexCharsCounterInit(seed, sizeof(seed))) {
            ERRR("counter init accepted a short seed or refused a full one");
        }
        uint64_t c1 = 7;
        uint64_t c2 = 7;
        uint8_t a[50];
        uint8_t b[50];
        getRandomHexCharsCounter(seed, &c1, a, sizeof(a));
        getRandomHexCharsCounter(seed, &c2, b, sizeof(b));
        bool hex = true;
        for (size_t i = 0; i < sizeof(a); i++) {
            hex &= (a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'f');
        }
        if (!hex || memcmp(a, b, sizeof(a)) != 0 || c1 != c2 || c1 == 7 ||
            getRandomHexCharsCounter(NULL, &c1, a, 1)) {
            ERRR("counter-mode hex is not deterministic hex");
        }
    }

    TEST("humanToBytes") {
        bool errr;
        const uint64_t GB12 = humanToBytes("12GB", 4, &errr);
        assert(!errr);
        assert(GB12 == (12ULL * 1000 * 1000 * 1000));

        const uint64_t GB_12 = humanToBytes("12 GB", 5, &errr);
        assert(!errr);
        assert(GB_12 == (12ULL * 1000 * 1000 * 1000));

        const uint64_t GB__12 = humanToBytes("12   GB", 7, &errr);
        assert(!errr);
        assert(GB__12 == (12ULL * 1000 * 1000 * 1000));
    }

    TEST("humanToBytes unit boundaries") {
        /* Written out independently of the parser's own table */
        const struct {
            const char *unit;
            uint64_t mul;
        } units[] = {
            {"", 1},
            {"b", 1},
            {"B", 1},
            {"k", 1000},
            {"KB", 1000},
            {"kib", 1024},
            {"m", 1000000},
            {"mb", 1000000},
            {"MiB", 1048576},
            {"g", 1000000000},
            {"gb", 1000000000},
            {"gib", 1073741824},
            {"t", 1000000000000},
            {"TB", 1000000000000},
            {"tib", 1099511627776},
            {"p", 1000000000000000},
            {"pb", 1000000000000000},
            {"pib", 1125899906842624},
            {"e", 1000000000000000000},
            {"eb", 1000000000000000000},
            {"EiB", 1152921504606846976},
        };

        for (size_t i = 0; i < sizeof(units) / sizeof(*units); i++) {
            const uint64_t max = UINT64_MAX / units[i].mul;
            char buf[64];
            bool e = true;

            /* The largest count that fits is accepted exactly */
            int n =
                snprintf(buf, sizeof(buf), "%" PRIu64 "%s", max, units[i].unit);
            uint64_t got = humanToBytes(buf, (size_t)n, &e);
            if (e || got != max * units[i].mul) {
                ERR("%s: got %" PRIu64 " err=%d, expected %" PRIu64, buf, got,
                    e, max * units[i].mul);
            }

            /* One more is refused, never wrapped. For bytes, max + 1 is
             * 2^64, which uint64_t cannot print; it is spelled out. */
            if (max == UINT64_MAX) {
                n = snprintf(buf, sizeof(buf), "18446744073709551616%s",
                             units[i].unit);
            } else {
                n = snprintf(buf, sizeof(buf), "%" PRIu64 "%s", max + 1,
                             units[i].unit);
            }
            e = false;
            got = humanToBytes(buf, (size_t)n, &e);
            if (!e || got != 0) {
                ERR("%s: got %" PRIu64 " err=%d, expected refusal", buf, got,
                    e);
            }
        }

        /* Units no uint64_t can hold, unknown units, digits that overflow
         * on their own, and a missing number are all refused */
        const char *refused[] = {"0zb",
                                 "1z",
                                 "1zb",
                                 "1zib",
                                 "1ZiB",
                                 "1y",
                                 "1yb",
                                 "1yib",
                                 "1q",
                                 "1 kbb",
                                 "gb",
                                 "",
                                 "18446744073709551616"};
        for (size_t i = 0; i < sizeof(refused) / sizeof(*refused); i++) {
            bool e = false;
            const uint64_t got =
                humanToBytes(refused[i], strlen(refused[i]), &e);
            if (!e || got != 0) {
                ERR("\"%s\": got %" PRIu64 " err=%d, expected refusal",
                    refused[i], got, e);
            }
        }

        /* Only the first 'len' bytes count */
        bool e = true;
        if (humanToBytes("12GBxyz", 4, &e) != 12000000000ULL || e) {
            ERRR("len-bounded \"12GB\" should parse");
        }
    }

    TEST("bytesToHuman") {
        const uint64_t EiB = 1ULL << 60;
        const struct {
            uint64_t n;
            const char *want;
        } cases[] = {
            {0, "0 B"},
            {1023, "1023 B"},
            {1024, "1.00000 KiB"},
            {3ULL << 29, "1.50000 GiB"},
            {3ULL << 49, "1.50000 PiB"},
            {EiB, "1.00000 EiB"},
            {3 * EiB, "3.00000 EiB"},
            {UINT64_MAX, "16.00000 EiB"},
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
            char buf[64];
            const bool ok = bytesToHuman(buf, sizeof(buf), cases[i].n);
            if (!ok || strcmp(buf, cases[i].want) != 0) {
                ERR("bytesToHuman(%" PRIu64 ") = %s (%d), expected %s",
                    cases[i].n, buf, ok, cases[i].want);
            }
        }
    }

    TEST("pow2Ceiling64 stress test") {
        printf("  Testing pow2Ceiling64 against reference...\n");

        /* Test edge cases */
        const uint64_t edgeCases[] = {
            0,
            1,
            2,
            3,
            4,
            5,
            7,
            8,
            9,
            15,
            16,
            17,
            31,
            32,
            33,
            63,
            64,
            65,
            127,
            128,
            129,
            255,
            256,
            257,
            511,
            512,
            513,
            1023,
            1024,
            1025,
            (1ULL << 16) - 1,
            (1ULL << 16),
            (1ULL << 16) + 1,
            (1ULL << 32) - 1,
            (1ULL << 32),
            (1ULL << 32) + 1,
            (1ULL << 62) - 1,
            (1ULL << 62),
            (1ULL << 62) + 1,
            (1ULL << 63) - 1,
        };

        for (size_t i = 0; i < sizeof(edgeCases) / sizeof(edgeCases[0]); i++) {
            uint64_t x = edgeCases[i];
            uint64_t result = pow2Ceiling64(x);
            uint64_t expected = pow2Ceiling64_reference(x);
            if (result != expected) {
                ERR("pow2Ceiling64 mismatch for %" PRIu64 ": got %" PRIu64
                    ", expected %" PRIu64,
                    x, result, expected);
            }
        }

        /* Test all values around powers of 2 */
        for (int shift = 1; shift < 63; shift++) {
            uint64_t powerOf2 = 1ULL << shift;
            for (int64_t offset = -2; offset <= 2; offset++) {
                uint64_t x = powerOf2 + offset;
                if (x == 0) {
                    continue; /* Skip underflow case */
                }

                uint64_t result = pow2Ceiling64(x);
                uint64_t expected = pow2Ceiling64_reference(x);
                if (result != expected) {
                    ERR("pow2Ceiling64 power-of-2 mismatch for %" PRIu64
                        " (2^%d + %" PRId64 "): got %" PRIu64
                        ", expected %" PRIu64,
                        x, shift, offset, result, expected);
                }
            }
        }

        /* Random stress test */
        uint64_t rngState = 0xDEADBEEF12345678ULL;
        for (int i = 0; i < 100000; i++) {
            rngState = rngState * 6364136223846793005ULL + 1;
            uint64_t x = rngState;

            /* Also test smaller values more frequently */
            if (i % 3 == 0) {
                x = x % (1ULL << 32);
            } else if (i % 3 == 1) {
                x = x % (1ULL << 16);
            }

            uint64_t result = pow2Ceiling64(x);
            uint64_t expected = pow2Ceiling64_reference(x);
            if (result != expected) {
                ERR("pow2Ceiling64 random mismatch for %" PRIu64
                    ": got %" PRIu64 ", expected %" PRIu64,
                    x, result, expected);
            }
        }

        printf("    pow2Ceiling64 stress test passed!\n");
    }

    return err;
}
#endif

/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2016-2019, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
