#pragma once

/* dkDiag: how datakit reports misuse and conditions without printing.
 *
 * datakit runs inside other applications, which own stdout and stderr, so
 * no data structure writes to them on its own.  Instead, where a call is
 * refused for misuse (a value-mode roar given to a bit operation, a context
 * held by another wrapper), where a runtime condition is worth knowing (a
 * WAL left without its snapshot), or just before an abort for misuse that
 * cannot be continued past, the reporting site records a diagnostic:
 *
 *   - into the calling thread's slot, which the caller reads on demand with
 *     dkDiagLast (and dkDiagCount says whether anything new was recorded);
 *   - to the process hook, if the application installed one (to log it);
 *   - to stderr only when the print mode asks: by default in DATAKIT_TEST
 *     builds and never otherwise, overridable at runtime either way.
 *
 * Only refusal and failure branches report, so no success path pays for
 * any of this.  The normal API is unchanged: functions still return their
 * false / NULL / 0; the diagnostic says why.
 *
 * Severity:
 *   MISUSE     the API was called wrongly; refused, the program goes on
 *   CONDITION  a runtime state worth knowing; not the caller's mistake
 *   INTERNAL   a datakit invariant failed (a self-test check)
 *   FATAL      misuse that cannot be continued past; abort() follows the
 *              report (the hook runs first, so an application can log it)
 *
 * Codes are append-only: a value is never reused or renumbered, so an
 * application may log and match them. */

#include <stdbool.h>
#include <stdint.h>

typedef enum dkDiagSeverity {
    DK_DIAG_MISUSE = 1,
    DK_DIAG_CONDITION = 2,
    DK_DIAG_INTERNAL = 3,
    DK_DIAG_FATAL = 4,
} dkDiagSeverity;

typedef enum dkDiagCode {
    DK_DIAG_NONE = 0,
    /* flex was given a box type it has no encoding for (FATAL) */
    DK_DIAG_FLEX_BOX_UNENCODABLE = 1,
    /* a deferred wide box (type only, no payload) was used as a value
     * (FATAL) */
    DK_DIAG_DATABOX_WIDE_DEFERRED = 2,
    /* an offsetArray index outside the array (FATAL, OFFSETARRAY_DEBUG) */
    DK_DIAG_OFFSETARRAY_BOUNDS = 3,
    /* a datakit self-test check failed (INTERNAL) */
    DK_DIAG_SELF_TEST_FAILED = 4,
    /* a persist WAL exists without its snapshot (CONDITION) */
    DK_DIAG_PERSIST_ORPHAN_WAL = 5,
    /* a persistCtx bound to one structure type was used for another */
    DK_DIAG_PERSIST_CTX_WRONG_TYPE = 6,
    /* a persistCtx held by a live wrapper was given to a second one */
    DK_DIAG_PERSIST_CTX_CLAIMED = 7,
    /* a multiroar operation for the other mode (bit / value) */
    DK_DIAG_MULTIROAR_WRONG_MODE = 8,
    /* a container operation on a native multiarray */
    DK_DIAG_MULTIARRAY_NATIVE = 9,
    /* two numeric boxes with no order between them (a VOID box) (FATAL) */
    DK_DIAG_DATABOX_UNCOMPARABLE = 10,
    /* an owned cell or sequence published by a writer its declaration
     * does not allow (counted, never refused) */
    DK_DIAG_OWNED_WRITER_VIOLATION = 11,
    /* an ownedSlot published with no live claim behind it */
    DK_DIAG_OWNED_SLOT_UNCLAIMED = 12,
    /* an AEAD call given NULL buffers for a nonzero length, or a stream
     * call out of order (or on a context already finished) */
    DK_DIAG_AEAD_MISUSE = 13,
    /* an hmacKeyring salted install given a NULL or all-zero salt */
    DK_DIAG_HMAC_KEYRING_ZERO_SALT = 14,
    /* dkRandomBounded(0): a value from an empty interval */
    DK_DIAG_RANDOM_ZERO_BOUND = 15,
    /* a membound free of a pointer the pool does not hold out (outside
     * it, misaligned, freed already, or into a block's interior) */
    DK_DIAG_MEMBOUND_INVALID_FREE = 16,
    /* a timer callback called its timer's ProcessTimerEvents (ignored) */
    DK_DIAG_TIMER_REENTERED = 17,
    /* a multiarray index (or range) outside its elements: a Get answers
     * NULL, a change is refused */
    DK_DIAG_MULTIARRAY_INDEX = 18,
    /* an in-place rewrite (replace, resize, increment) of a set map's row:
     * every element of a set row orders it, so the rewrite is refused */
    DK_DIAG_MULTIMAP_SET_REWRITE = 19,
    DK_DIAG_CODE_COUNT_ /* one past the last code; not a code */
} dkDiagCode;

typedef struct dkDiagnostic {
    uint64_t seq; /* the thread's count when recorded (first: 1) */
    dkDiagCode code;
    dkDiagSeverity severity;
    const char *function; /* the reporting site's __func__ */
    const char *file;     /* its __FILE__, as the compiler spelled it */
    uint32_t line;
    char message[160]; /* formatted detail, truncated to fit */
} dkDiagnostic;

/* ---- Discovery: pull ---------------------------------------------------
 * The calling thread's last diagnostic into 'out'; false when this thread
 * has none (none recorded, or cleared since). */
bool dkDiagLast(dkDiagnostic *out);

/* The calling thread's diagnostic count: compare it before and after a
 * call to learn whether that call reported anything. */
uint64_t dkDiagCount(void);

/* Forget the calling thread's last diagnostic (the count keeps running) */
void dkDiagClear(void);

/* Stable names, for logs: "MULTIROAR_WRONG_MODE", "misuse"; "unknown" for
 * a value outside the enum */
const char *dkDiagCodeName(dkDiagCode code);
const char *dkDiagSeverityName(dkDiagSeverity severity);

/* ---- Discovery: push ---------------------------------------------------
 * One process-wide hook, called on the reporting thread with a copy of
 * the diagnostic (before abort() for FATAL).  The struct is the caller's
 * and must outlive every report that might use it; NULL removes the hook.
 * The hook may call dkDiagLast; a report made inside it is recorded and
 * hooked as usual.  A report can come from inside a structure's lock (a
 * membound pool's free path, for one), so the hook must not call back into
 * the structure whose call is reporting: log, count, or hand off. */
typedef struct dkDiagHook {
    void (*fn)(const dkDiagnostic *d, void *ctx);
    void *ctx;
} dkDiagHook;
void dkDiagSetHook(const dkDiagHook *hook);

/* ---- Printing ----------------------------------------------------------
 * Whether a report also writes one line to stderr.  DEFAULT prints in
 * DATAKIT_TEST builds and stays silent otherwise; ALWAYS and NEVER
 * override that in either kind of build. */
typedef enum dkDiagPrintMode {
    DK_DIAG_PRINT_DEFAULT = 0,
    DK_DIAG_PRINT_ALWAYS = 1,
    DK_DIAG_PRINT_NEVER = 2,
} dkDiagPrintMode;
void dkDiagSetPrintMode(dkDiagPrintMode mode);
dkDiagPrintMode dkDiagGetPrintMode(void);

/* ---- Reporting (datakit's own sites) -----------------------------------
 * The macros pass the site's location; the format is printf's. */
#define DK_DIAG(code, ...)                                                     \
    dkDiagReport_(DK_DIAG_MISUSE, (code), __func__, __FILE__, __LINE__,        \
                  __VA_ARGS__)
#define DK_DIAG_CONDITION(code, ...)                                           \
    dkDiagReport_(DK_DIAG_CONDITION, (code), __func__, __FILE__, __LINE__,     \
                  __VA_ARGS__)
#define DK_DIAG_INTERNAL(code, ...)                                            \
    dkDiagReport_(DK_DIAG_INTERNAL, (code), __func__, __FILE__, __LINE__,      \
                  __VA_ARGS__)
#define DK_DIAG_FATAL(code, ...)                                               \
    dkDiagFatal_((code), __func__, __FILE__, __LINE__, __VA_ARGS__)

__attribute__((cold, noinline, format(printf, 6, 7))) void
dkDiagReport_(dkDiagSeverity severity, dkDiagCode code, const char *function,
              const char *file, int line, const char *fmt, ...);
__attribute__((cold, noinline, noreturn, format(printf, 5, 6))) void
dkDiagFatal_(dkDiagCode code, const char *function, const char *file, int line,
             const char *fmt, ...);

#ifdef DATAKIT_TEST
int dkDiagTest(int argc, char *argv[]);

/* For tests (ctest.h's ERR and an 'err' in scope): 'call' records exactly
 * 'want' diagnostics, the last of them 'wantCode'; 'subject' names what
 * it ran on when it does not */
#define DK_DIAG_EXPECT_REPORTS(subject, want, wantCode, call)                  \
    do {                                                                       \
        const uint64_t c0_ = dkDiagCount();                                    \
        call;                                                                  \
        const uint64_t got_ = dkDiagCount() - c0_;                             \
        dkDiagnostic d_;                                                       \
        if (got_ != (want) ||                                                  \
            ((want) && (!dkDiagLast(&d_) || d_.code != (wantCode)))) {         \
            ERR("%s on %s reported %" PRIu64 " times, want %" PRIu64, #call,   \
                (subject), got_, (uint64_t)(want));                            \
        }                                                                      \
    } while (0)
#endif
