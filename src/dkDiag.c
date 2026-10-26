#include "dkDiag.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The calling thread's last diagnostic and its count.  Static TLS: no
 * allocation, so reporting works on allocation-failure paths too, and
 * nothing to clean up at thread exit. */
static thread_local dkDiagnostic dkDiagSlot_;
static thread_local uint64_t dkDiagSeq_;

/* The process hook (a caller-owned {fn, ctx}: one pointer publishes both)
 * and the print mode */
static _Atomic(const dkDiagHook *) dkDiagHook_;
static _Atomic int dkDiagPrintMode_;

static const char *const dkDiagCodeNames_[DK_DIAG_CODE_COUNT_] = {
    [DK_DIAG_NONE] = "NONE",
    [DK_DIAG_FLEX_BOX_UNENCODABLE] = "FLEX_BOX_UNENCODABLE",
    [DK_DIAG_DATABOX_WIDE_DEFERRED] = "DATABOX_WIDE_DEFERRED",
    [DK_DIAG_OFFSETARRAY_BOUNDS] = "OFFSETARRAY_BOUNDS",
    [DK_DIAG_SELF_TEST_FAILED] = "SELF_TEST_FAILED",
    [DK_DIAG_PERSIST_ORPHAN_WAL] = "PERSIST_ORPHAN_WAL",
    [DK_DIAG_PERSIST_CTX_WRONG_TYPE] = "PERSIST_CTX_WRONG_TYPE",
    [DK_DIAG_PERSIST_CTX_CLAIMED] = "PERSIST_CTX_CLAIMED",
    [DK_DIAG_MULTIROAR_WRONG_MODE] = "MULTIROAR_WRONG_MODE",
    [DK_DIAG_MULTIARRAY_NATIVE] = "MULTIARRAY_NATIVE",
    [DK_DIAG_DATABOX_UNCOMPARABLE] = "DATABOX_UNCOMPARABLE",
    [DK_DIAG_OWNED_WRITER_VIOLATION] = "OWNED_WRITER_VIOLATION",
    [DK_DIAG_OWNED_SLOT_UNCLAIMED] = "OWNED_SLOT_UNCLAIMED",
    [DK_DIAG_AEAD_MISUSE] = "AEAD_MISUSE",
    [DK_DIAG_HMAC_KEYRING_ZERO_SALT] = "HMAC_KEYRING_ZERO_SALT",
    [DK_DIAG_RANDOM_ZERO_BOUND] = "RANDOM_ZERO_BOUND",
    [DK_DIAG_MEMBOUND_INVALID_FREE] = "MEMBOUND_INVALID_FREE",
    [DK_DIAG_TIMER_REENTERED] = "TIMER_REENTERED",
    [DK_DIAG_MULTIARRAY_INDEX] = "MULTIARRAY_INDEX",
    [DK_DIAG_MULTIMAP_SET_REWRITE] = "MULTIMAP_SET_REWRITE",
};

const char *dkDiagCodeName(dkDiagCode code) {
    if ((unsigned)code >= DK_DIAG_CODE_COUNT_ || !dkDiagCodeNames_[code]) {
        return "unknown";
    }
    return dkDiagCodeNames_[code];
}

const char *dkDiagSeverityName(dkDiagSeverity severity) {
    switch (severity) {
    case DK_DIAG_MISUSE:
        return "misuse";
    case DK_DIAG_CONDITION:
        return "condition";
    case DK_DIAG_INTERNAL:
        return "internal";
    case DK_DIAG_FATAL:
        return "fatal";
    }
    return "unknown";
}

bool dkDiagLast(dkDiagnostic *out) {
    if (dkDiagSlot_.seq == 0) {
        return false;
    }
    if (out) {
        *out = dkDiagSlot_;
    }
    return true;
}

uint64_t dkDiagCount(void) {
    return dkDiagSeq_;
}

void dkDiagClear(void) {
    memset(&dkDiagSlot_, 0, sizeof(dkDiagSlot_));
}

void dkDiagSetHook(const dkDiagHook *hook) {
    /* @sync: release diag.hook */
    atomic_store_explicit(&dkDiagHook_, hook, memory_order_release);
}

void dkDiagSetPrintMode(dkDiagPrintMode mode) {
    /* @sync: relaxed hint */
    atomic_store_explicit(&dkDiagPrintMode_, (int)mode, memory_order_relaxed);
}

dkDiagPrintMode dkDiagGetPrintMode(void) {
    /* @sync: relaxed hint */
    return (dkDiagPrintMode)atomic_load_explicit(&dkDiagPrintMode_,
                                                 memory_order_relaxed);
}

/* Whether a report writes a line to stderr: a report racing a mode change
 * may follow either mode */
static bool dkDiagShouldPrint_(void) {
    switch (dkDiagGetPrintMode()) {
    case DK_DIAG_PRINT_ALWAYS:
        return true;
    case DK_DIAG_PRINT_NEVER:
        return false;
    case DK_DIAG_PRINT_DEFAULT:
        break;
    }
#ifdef DATAKIT_TEST
    return true;
#else
    return false;
#endif
}

/* The one line a report prints, only when the print mode asks for it */
static void dkDiagPrint_(const dkDiagnostic *d) {
    const char *base = strrchr(d->file, '/');
    fprintf(stderr, "datakit: %s: %s in %s (%s:%" PRIu32 "): %s\n",
            dkDiagSeverityName(d->severity), dkDiagCodeName(d->code),
            d->function, base ? base + 1 : d->file, d->line, d->message);
}

static void dkDiagRecord_(dkDiagSeverity severity, dkDiagCode code,
                          const char *function, const char *file, int line,
                          const char *fmt, va_list ap) {
    /* Built on the stack, then stored: the hook gets this copy, so a
     * report made inside the hook (which overwrites the slot) cannot
     * change the diagnostic the hook is reading */
    dkDiagnostic d;
    memset(&d, 0, sizeof(d));
    d.seq = ++dkDiagSeq_;
    d.code = code;
    d.severity = severity;
    d.function = function;
    d.file = file;
    d.line = line < 0 ? 0 : (uint32_t)line;
    vsnprintf(d.message, sizeof(d.message), fmt, ap);
    dkDiagSlot_ = d;

    /* @sync: acquire diag.hook */
    const dkDiagHook *hook =
        atomic_load_explicit(&dkDiagHook_, memory_order_acquire);
    if (hook && hook->fn) {
        hook->fn(&d, hook->ctx);
    }

    if (dkDiagShouldPrint_()) {
        dkDiagPrint_(&d);
    }
}

void dkDiagReport_(dkDiagSeverity severity, dkDiagCode code,
                   const char *function, const char *file, int line,
                   const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    dkDiagRecord_(severity, code, function, file, line, fmt, ap);
    va_end(ap);
}

void dkDiagFatal_(dkDiagCode code, const char *function, const char *file,
                  int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    dkDiagRecord_(DK_DIAG_FATAL, code, function, file, line, fmt, ap);
    va_end(ap);
    abort();
}

#ifdef DATAKIT_TEST
#include "ctest.h"

#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* What the test hook saw */
typedef struct dkDiagTestSeen {
    int calls;
    dkDiagnostic last;
    bool nested;     /* report again from inside the hook, once */
    bool copyStable; /* the hook's copy survived its own nested report */
} dkDiagTestSeen;

static void dkDiagTestHook_(const dkDiagnostic *d, void *ctx) {
    dkDiagTestSeen *seen = ctx;
    seen->calls++;
    seen->last = *d;
    if (seen->nested) {
        seen->nested = false;
        const uint64_t before = d->seq;
        DK_DIAG(DK_DIAG_MULTIARRAY_NATIVE, "nested report");
        seen->copyStable =
            d->seq == before && d->code == DK_DIAG_MULTIROAR_WRONG_MODE;
    }
}

static void *dkDiagTestThread_(void *arg) {
    uint64_t *out = arg;
    DK_DIAG(DK_DIAG_PERSIST_CTX_CLAIMED, "from another thread");
    dkDiagnostic d;
    out[0] = dkDiagCount();
    out[1] = dkDiagLast(&d) && d.code == DK_DIAG_PERSIST_CTX_CLAIMED;
    return NULL;
}

/* Everything written to stderr while 'mode' is in effect and one MISUSE
 * report runs, into 'buf' (NUL-terminated); its length */
static size_t dkDiagTestCaptureStderr_(dkDiagPrintMode mode, char *buf,
                                       size_t cap) {
    int fds[2];
    if (pipe(fds) != 0) {
        return SIZE_MAX;
    }
    fflush(stderr);
    const int saved = dup(STDERR_FILENO);
    dup2(fds[1], STDERR_FILENO);
    const dkDiagPrintMode prior = dkDiagGetPrintMode();
    dkDiagSetPrintMode(mode);
    DK_DIAG(DK_DIAG_MULTIROAR_WRONG_MODE, "captured %d", 7);
    dkDiagSetPrintMode(prior);
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    close(fds[1]);
    size_t got = 0;
    ssize_t n;
    while (got + 1 < cap && (n = read(fds[0], buf + got, cap - 1 - got)) > 0) {
        got += (size_t)n;
    }
    close(fds[0]);
    buf[got] = '\0';
    return got;
}

#define DK_DIAG_TEST_ABORTED 87
[[noreturn]] static void dkDiagTestAbortHandler_(int sig) {
    (void)sig;
    _exit(DK_DIAG_TEST_ABORTED);
}

static void dkDiagTestFatalHook_(const dkDiagnostic *d, void *ctx) {
    const int fd = *(const int *)ctx;
    const uint32_t code = (uint32_t)d->code;
    (void)!write(fd, &code, sizeof(code));
}

int dkDiagTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    /* The suite's own reports stay off stderr unless it captures them */
    dkDiagSetPrintMode(DK_DIAG_PRINT_NEVER);

    TEST("a report records code, severity, site and message") {
        const uint64_t before = dkDiagCount();
        const int line = __LINE__ + 1;
        DK_DIAG(DK_DIAG_MULTIROAR_WRONG_MODE, "roar %d in %s", 3, "bit op");
        dkDiagnostic d;
        if (dkDiagCount() != before + 1 || !dkDiagLast(&d) ||
            d.seq != before + 1 || d.code != DK_DIAG_MULTIROAR_WRONG_MODE ||
            d.severity != DK_DIAG_MISUSE || d.line != (uint32_t)line ||
            strcmp(d.function, "dkDiagTest") != 0 ||
            strcmp(d.message, "roar 3 in bit op") != 0 ||
            !strstr(d.file, "dkDiag.c")) {
            ERR("recorded seq %" PRIu64 " code %d line %" PRIu32 " '%s'", d.seq,
                (int)d.code, d.line, d.message);
        }

        DK_DIAG_CONDITION(DK_DIAG_PERSIST_ORPHAN_WAL, "c");
        dkDiagLast(&d);
        const bool condition = d.severity == DK_DIAG_CONDITION;
        DK_DIAG_INTERNAL(DK_DIAG_SELF_TEST_FAILED, "i");
        dkDiagLast(&d);
        if (!condition || d.severity != DK_DIAG_INTERNAL ||
            dkDiagCount() != before + 3) {
            ERR("condition / internal severities");
        }
    }

    TEST("clear forgets the slot; the count keeps running") {
        DK_DIAG(DK_DIAG_MULTIARRAY_NATIVE, "x");
        const uint64_t count = dkDiagCount();
        dkDiagClear();
        dkDiagnostic d;
        if (dkDiagLast(&d) || dkDiagLast(NULL) || dkDiagCount() != count) {
            ERR("clear left a diagnostic or moved the count");
        }
        DK_DIAG(DK_DIAG_MULTIARRAY_NATIVE, "y");
        if (!dkDiagLast(NULL) || dkDiagCount() != count + 1) {
            ERR("a report after clear was not recorded");
        }
    }

    TEST("a long message truncates to fit") {
        char longText[400];
        memset(longText, 'q', sizeof(longText) - 1);
        longText[sizeof(longText) - 1] = '\0';
        DK_DIAG(DK_DIAG_MULTIARRAY_NATIVE, "%s", longText);
        dkDiagnostic d;
        dkDiagLast(&d);
        if (strlen(d.message) != sizeof(d.message) - 1 || d.message[0] != 'q') {
            ERR("message length %zu", strlen(d.message));
        }
    }

    TEST("the hook sees each report with its context, then can be removed") {
        dkDiagTestSeen seen = {};
        const dkDiagHook hook = {.fn = dkDiagTestHook_, .ctx = &seen};
        dkDiagSetHook(&hook);
        DK_DIAG(DK_DIAG_PERSIST_CTX_WRONG_TYPE, "hooked");
        const bool sawOne = seen.calls == 1 &&
                            seen.last.code == DK_DIAG_PERSIST_CTX_WRONG_TYPE &&
                            seen.last.seq == dkDiagCount();

        /* A report from inside the hook: recorded and hooked too, and the
         * outer report's copy stays as it was */
        seen.nested = true;
        DK_DIAG(DK_DIAG_MULTIROAR_WRONG_MODE, "outer");
        dkDiagnostic d;
        dkDiagLast(&d);
        const bool nestedOk = seen.calls == 3 && seen.copyStable &&
                              d.code == DK_DIAG_MULTIARRAY_NATIVE;

        dkDiagSetHook(NULL);
        DK_DIAG(DK_DIAG_PERSIST_CTX_WRONG_TYPE, "unhooked");
        if (!sawOne || !nestedOk || seen.calls != 3) {
            ERR("hook calls %d, first ok %d, nested ok %d", seen.calls, sawOne,
                nestedOk);
        }
    }

    TEST("each thread has its own slot and count") {
        DK_DIAG(DK_DIAG_MULTIARRAY_NATIVE, "main thread");
        const uint64_t mine = dkDiagCount();
        uint64_t theirs[2] = {};
        pthread_t t;
        if (pthread_create(&t, NULL, dkDiagTestThread_, theirs) != 0) {
            ERR("pthread_create failed");
        } else {
            pthread_join(t, NULL);
            dkDiagnostic d;
            dkDiagLast(&d);
            if (theirs[0] != 1 || theirs[1] != 1 || dkDiagCount() != mine ||
                d.code != DK_DIAG_MULTIARRAY_NATIVE) {
                ERR("thread count %" PRIu64 " (want 1), main %" PRIu64
                    " (want %" PRIu64 ")",
                    theirs[0], dkDiagCount(), mine);
            }
        }
    }

    TEST("every code and severity has a name; others are unknown") {
        for (int c = 0; c < DK_DIAG_CODE_COUNT_; c++) {
            const char *name = dkDiagCodeName((dkDiagCode)c);
            if (strcmp(name, "unknown") == 0) {
                ERR("code %d has no name", c);
            }
            for (int o = 0; o < c; o++) {
                if (strcmp(name, dkDiagCodeName((dkDiagCode)o)) == 0) {
                    ERR("codes %d and %d share the name %s", o, c, name);
                }
            }
        }
        if (strcmp(dkDiagCodeName(DK_DIAG_CODE_COUNT_), "unknown") != 0 ||
            strcmp(dkDiagCodeName((dkDiagCode)-1), "unknown") != 0 ||
            strcmp(dkDiagSeverityName(DK_DIAG_FATAL), "fatal") != 0 ||
            strcmp(dkDiagSeverityName((dkDiagSeverity)0), "unknown") != 0) {
            ERR("out-of-range names");
        }
    }

    TEST("the print mode decides whether a report writes to stderr") {
        char out[512];
        const size_t always =
            dkDiagTestCaptureStderr_(DK_DIAG_PRINT_ALWAYS, out, sizeof(out));
        const bool alwaysOk =
            always != SIZE_MAX &&
            strstr(out, "datakit: misuse: MULTIROAR_WRONG_MODE in "
                        "dkDiagTestCaptureStderr_ (dkDiag.c:") &&
            strstr(out, "): captured 7\n");
        const size_t never =
            dkDiagTestCaptureStderr_(DK_DIAG_PRINT_NEVER, out, sizeof(out));
        /* DEFAULT prints in this (test) build */
        const size_t byDefault =
            dkDiagTestCaptureStderr_(DK_DIAG_PRINT_DEFAULT, out, sizeof(out));
        if (!alwaysOk || never != 0 || byDefault == 0 ||
            byDefault == SIZE_MAX) {
            ERR("always %zu (ok %d), never %zu, default %zu", always, alwaysOk,
                never, byDefault);
        }
        dkDiagSetPrintMode(DK_DIAG_PRINT_ALWAYS);
        const bool roundTrip = dkDiagGetPrintMode() == DK_DIAG_PRINT_ALWAYS;
        dkDiagSetPrintMode(DK_DIAG_PRINT_NEVER);
        if (!roundTrip || dkDiagGetPrintMode() != DK_DIAG_PRINT_NEVER) {
            ERR("print mode get/set");
        }
    }

    TEST("a fatal report runs the hook, then aborts") {
        int fds[2];
        if (pipe(fds) != 0) {
            ERR("pipe failed");
        } else {
            fflush(stdout);
            fflush(stderr);
            const pid_t kid = fork();
            if (kid == 0) {
                (void)signal(SIGABRT, dkDiagTestAbortHandler_);
                close(fds[0]);
                const dkDiagHook hook = {.fn = dkDiagTestFatalHook_,
                                         .ctx = &fds[1]};
                dkDiagSetHook(&hook);
                DK_DIAG_FATAL(DK_DIAG_FLEX_BOX_UNENCODABLE, "type %d", 99);
            }
            close(fds[1]);
            uint32_t code = 0;
            const ssize_t got = read(fds[0], &code, sizeof(code));
            close(fds[0]);
            int status = 0;
            const bool reaped = kid > 0 && waitpid(kid, &status, 0) == kid;
            if (!reaped || !WIFEXITED(status) ||
                WEXITSTATUS(status) != DK_DIAG_TEST_ABORTED ||
                got != (ssize_t)sizeof(code) ||
                code != DK_DIAG_FLEX_BOX_UNENCODABLE) {
                ERR("child status %d, hook code %" PRIu32 " (%zd bytes)",
                    status, code, got);
            }
        }
    }

    dkDiagSetPrintMode(DK_DIAG_PRINT_DEFAULT);
    TEST_FINAL_RESULT;
}
#endif
