# Diagnostics (dkDiag)

datakit runs inside your application, and your application owns stdout and stderr, so no datakit data
structure prints anything on its own.  When a call is refused because it was made wrongly, when a runtime
condition is worth knowing about, or just before an abort for misuse that cannot be continued past, datakit
records a **diagnostic** instead.  You read it when you want it; nothing else changes.

Header: `src/dkDiag.h`.

## What gets reported

| severity | meaning | the call |
| --- | --- | --- |
| `DK_DIAG_MISUSE` | the API was called wrongly | refused as documented (false / NULL / 0), program goes on |
| `DK_DIAG_CONDITION` | a runtime state worth knowing, not your mistake | returns normally |
| `DK_DIAG_INTERNAL` | a datakit check failed (a self-test) | returns its failure count |
| `DK_DIAG_FATAL` | misuse that cannot be continued past | `abort()` after the report |

Codes (one enum, append-only: a value is never reused, so you may log and match them):

| code | severity | reported by |
| --- | --- | --- |
| `DK_DIAG_FLEX_BOX_UNENCODABLE` | FATAL | a flex store given a box type flex has no encoding for |
| `DK_DIAG_DATABOX_WIDE_DEFERRED` | FATAL | a deferred wide box (type only, no payload) used as a value |
| `DK_DIAG_OFFSETARRAY_BOUNDS` | FATAL | `offsetArrayGet` outside the array (OFFSETARRAY_DEBUG builds) |
| `DK_DIAG_SELF_TEST_FAILED` | INTERNAL | a failed check of `flexFactorySelfTest` |
| `DK_DIAG_PERSIST_ORPHAN_WAL` | CONDITION | `persistCtxExists` finding a WAL without its snapshot |
| `DK_DIAG_PERSIST_CTX_WRONG_TYPE` | MISUSE | a persistCtx bound to one structure type used for another |
| `DK_DIAG_PERSIST_CTX_CLAIMED` | MISUSE | a second wrapper on a persistCtx held by a live one |
| `DK_DIAG_MULTIROAR_WRONG_MODE` | MISUSE | a bit operation on a value-mode roar, or a value operation on a bit roar |
| `DK_DIAG_MULTIARRAY_NATIVE` | MISUSE | a container call on a native multiarray |
| `DK_DIAG_DATABOX_UNCOMPARABLE` | FATAL | two numeric boxes with no order between them |

Not reported: NULL passed where NULL means "nothing" (`*Free(NULL)`, a NULL operand), data a structure
refuses because of its content (a value too wide for a roar, a position past its dimensions), and every
success path.

## Reading diagnostics

Each thread has its own last diagnostic and its own count.

```c
#include "dkDiag.h"

const uint64_t before = dkDiagCount();
if (!multiroarBitSet(r, 5)) {
    dkDiagnostic d;
    if (dkDiagCount() != before && dkDiagLast(&d)) {
        /* d.code == DK_DIAG_MULTIROAR_WRONG_MODE, d.severity == DK_DIAG_MISUSE,
         * d.function == "multiroarBitSet", d.message explains */
        log("%s: %s", dkDiagCodeName(d.code), d.message);
    }
}
```

- `dkDiagLast(&d)` copies the thread's last diagnostic; false when there is none.
- `dkDiagCount()` is the thread's running count: compare it before and after a call to know whether that
  call reported, without clearing anything other code may want.
- `dkDiagClear()` forgets the last diagnostic (the count keeps running), for errno-style use.
- `dkDiagCodeName` / `dkDiagSeverityName` give stable names ("MULTIROAR_WRONG_MODE", "misuse").

A diagnostic holds the code, severity, the reporting function, file and line, a sequence number and a
message of up to 159 characters (longer detail is truncated).

## Getting diagnostics pushed to you

To route every diagnostic into your own logging, install a hook once:

```c
static void onDiag(const dkDiagnostic *d, void *ctx) {
    myLog(ctx, "datakit %s %s in %s: %s", dkDiagSeverityName(d->severity),
          dkDiagCodeName(d->code), d->function, d->message);
}

static const dkDiagHook hook = {.fn = onDiag, .ctx = &myLogger};
dkDiagSetHook(&hook);   /* NULL removes it */
```

The hook runs on the reporting thread, with a copy of the diagnostic, and for FATAL before the abort, so a
fatal misuse reaches your logs.  The hook struct is yours and must outlive every report that might use it
(static storage is simplest).  A hook may call `dkDiagLast`; a report made inside the hook is recorded and
hooked as usual.

## Printing

Whether a report also writes one line to stderr is the print mode:

| mode | prints |
| --- | --- |
| `DK_DIAG_PRINT_DEFAULT` | in DATAKIT_TEST builds only (so development builds show misuse as it happens) |
| `DK_DIAG_PRINT_ALWAYS` | in every build (debugging a deployed program's use of datakit) |
| `DK_DIAG_PRINT_NEVER` | never (a test that misuses on purpose; a quiet embedder) |

```c
dkDiagSetPrintMode(DK_DIAG_PRINT_ALWAYS);
```

The line reads `datakit: misuse: MULTIROAR_WRONG_MODE in multiroarBitSet (multiroar.c:520): a bit operation
was given a value-mode roar`.  Recording and the hook happen in every mode.

## Cost

Only refusal and failure branches report, through a single out-of-line call; no success path does any
work for diagnostics.  The slot is static thread-local storage (no allocation, no cleanup at thread exit),
the hook is one atomic pointer, and the print mode is read only while reporting.

## Relation to other error channels

dkDiag is for conditions with no return value to carry them.  The typed per-object channels stay as they
are and are the first place to look for their own modules: `persistLastError` / `persistErr`,
`persistLastBadEntry` and the bad-entry notify callback, `flexEnvelopeLastVerifyError`, the cluster result
enums, and `persistCtxProbe` for what is on disk.

## Enforcement

`dktools outputaudit check` (ctest `libraryOutput`, and a step of `verify` and `validate`) fails while any
library code writes to stdout or stderr unasked.  The only library writer left is the dkDiag printer, which
runs when the print mode asks; repr/print functions, operator tools (dataspeed, memtest) and test code are
the other permitted writers.
