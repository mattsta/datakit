# SetProcTitle - Process Title Manipulation

## Overview

`setproctitle` changes the process name visible in system tools like `ps`,
`top`, and `htop`, which is useful for identifying server states, worker roles,
or debugging.

**Key Features:**

- Process title modification on Linux, macOS, and the BSD family
- Native `setproctitle(3)` used directly where the platform provides it (BSD)
- argv/environ relocation fallback on Linux and macOS
- Zero overhead on platforms with native support

**Header**: `setproctitle.h`

**Source**: `setproctitle.c`

**Platforms**: FreeBSD, NetBSD, OpenBSD (native), Linux and macOS (relocation
implementation). Everything else compiles to no-op macros.

**Origin**: Modified from nginx's `ngx_setproctitle` implementation (BSD-2 license)

## API Reference

The names are the same everywhere but **the shapes are not**. `setproctitle.h`
selects one of three variants at compile time:

| Platform group             | `setProctitleInit`                               | `setproctitle`                                                    |
| -------------------------- | ------------------------------------------------ | ----------------------------------------------------------------- |
| FreeBSD / NetBSD / OpenBSD | `#define setProctitleInit() true` (no arguments) | `#define setproctitle(t) setproctitle("%s", t)` - the native call |
| Linux / macOS              | `bool setProctitleInit(char *argv[])`            | `void setproctitle(char *title)`                                  |
| everything else            | `#define setProctitleInit() true` (no arguments) | `#define setproctitle(t)` - expands to nothing                    |

> **Portability trap:** on the BSD and fallback paths `setProctitleInit` is a
> **zero-argument macro**, so the natural `setProctitleInit(argv)` call is a
> compile error there ("too many arguments provided to function-like macro").
> Portable code must guard the init call:
>
> ```c
> #if DK_OS_LINUX || DK_OS_APPLE
>     setProctitleInit(argv);
> #else
>     setProctitleInit();
> #endif
> ```
>
> `setproctitle(title)` itself takes one argument in every variant and needs no
> guard.

### Initialization

```c
/* Initialize process title infrastructure (Linux / macOS)
 * argv: original argv from main()
 * Returns: true (this implementation has no failure path)
 *
 * Required before setproctitle() on Linux/macOS.
 * A no-argument macro returning true on BSD and unsupported platforms.
 */
bool setProctitleInit(char *argv[]);
```

**Important**: Call it early in `main()`, before anything modifies `argv` or
`environ`.

> **`setProctitleInit()` sets `argv[1] = NULL`.** That happens during _init_, not
> during the first `setproctitle()` call, so any argument parsing that walks
> `argv` must run **before** init - or copy the strings it needs first. `argc` is
> untouched, so an `argc`-driven loop run after init will read a NULL pointer.

### Setting Process Title

```c
/* Set process title
 * title: new process title string
 *
 * On BSD: forwarded to the native setproctitle("%s", title),
 *         which renders as "progname: title"
 * On Linux/macOS: overwrites the argv[0] region with title, then pads
 *                 the remainder of the reclaimed region with '\0'
 * Elsewhere: compiles to nothing
 */
void setproctitle(char *title);
```

On Linux/macOS the title is silently truncated to the space reclaimed by
`setProctitleInit()` (the argv region plus whatever contiguous environment
strings followed it). There is no error return and no length query.

## Platform Differences

### BSD Platforms (FreeBSD, NetBSD, OpenBSD)

BSD systems provide native `setproctitle()` support:

```c
/* On BSD, setProctitleInit() is a no-op macro */
#define setProctitleInit() true

/* Native setproctitle() called with format string */
#define setproctitle(title) setproctitle("%s", title)
```

**No initialization required** - the system provides the functionality. The
header's `setproctitle(title)` macro forwards to the native
`setproctitle("%s", title)` (a macro name is not re-expanded inside its own
expansion, so this resolves to the real libc function).

### Linux and macOS

These platforms require the relocation implementation:

```c
bool setProctitleInit(char *argv[]);
void setproctitle(char *title);
```

**How It Works:**

1. **Space Calculation**: Walks `argv[]` and then `environ[]`, following the
   contiguous run of strings that starts at `argv[0]`, to find the last usable
   byte (`os_argv_last`)
2. **Memory Relocation**: Copies each environment string that sits inside that
   run into freshly allocated memory and repoints `environ[i]` at the copy, so
   overwriting the region does not corrupt the environment
3. **argv truncation**: Sets `argv[1] = NULL`
4. **Title Storage**: `setproctitle()` copies the title over `argv[0]`, bounded
   by `os_argv_last`
5. **Padding**: Fills the remainder of the region with `\0`

`setproctitle()` asserts that initialization has run; in a build with assertions
disabled, calling it before `setProctitleInit()` dereferences NULL.

## Usage Examples

### Example 1: Basic Server

```c
#include "setproctitle.h"

int main(int argc, char *argv[]) {
    /* Parse arguments first - init nulls argv[1] */
    parseArgs(argc, argv);

    /* Then hand the argv region over (Linux/macOS form shown) */
    setProctitleInit(argv);

    /* Initial state */
    setproctitle("server: starting");

    /* ... initialization ... */

    /* Ready state */
    setproctitle("server: ready, port 8080");

    /* Main loop */
    while (running) {
        /* ... handle connections ... */
    }

    /* Shutdown */
    setproctitle("server: shutting down");
    cleanup();

    return 0;
}
```

**Before:**

```bash
$ ps aux | grep myserver
user  12345  myserver --port 8080 --workers 4
```

**After:**

```bash
$ ps aux | grep myserver
user  12345  server: ready, port 8080
```

### Example 2: Worker Pool with Status

```c
typedef enum WorkerState {
    WORKER_IDLE,
    WORKER_PROCESSING,
    WORKER_ERROR
} WorkerState;

void workerSetState(int workerID, WorkerState state, const char *detail) {
    char title[256];

    switch (state) {
    case WORKER_IDLE:
        snprintf(title, sizeof(title), "worker %d: idle", workerID);
        break;
    case WORKER_PROCESSING:
        snprintf(title, sizeof(title), "worker %d: processing %s",
                 workerID, detail);
        break;
    case WORKER_ERROR:
        snprintf(title, sizeof(title), "worker %d: error - %s",
                 workerID, detail);
        break;
    }

    setproctitle(title);
}

Job *getNextJob(void); /* your queue */

void workerMain(int workerID) {
    workerSetState(workerID, WORKER_IDLE, NULL);

    while (1) {
        Job *job = getNextJob();
        if (!job) {
            workerSetState(workerID, WORKER_IDLE, NULL);
            sleep(1);
            continue;
        }

        workerSetState(workerID, WORKER_PROCESSING, job->name);

        if (!processJob(job)) {
            workerSetState(workerID, WORKER_ERROR, job->error);
            sleep(5);
        }
    }
}
```

**Terminal output:**

```bash
$ ps aux | grep worker
user  12346  worker 0: idle
user  12347  worker 1: processing user-data-export.csv
user  12348  worker 2: idle
user  12349  worker 3: error - database connection failed
```

### Example 3: Connection Counter

```c
#include <stdatomic.h>

atomic_int activeConnections = 0;

void updateServerTitle(void) {
    char title[128];
    int conns = atomic_load(&activeConnections);

    snprintf(title, sizeof(title),
             "server: %d active connection%s",
             conns, conns == 1 ? "" : "s");

    setproctitle(title);
}

void handleClient(int clientFd) {
    atomic_fetch_add(&activeConnections, 1);
    updateServerTitle();

    /* ... handle client ... */

    atomic_fetch_sub(&activeConnections, 1);
    updateServerTitle();
}
```

### Example 4: Multi-Process Server

```c
int main(int argc, char *argv[]) {
    setProctitleInit(argv);

    /* Master process */
    setproctitle("master process");

    for (int i = 0; i < numWorkers; i++) {
        pid_t pid = fork();

        if (pid == 0) {
            /* Child worker */
            char title[64];
            snprintf(title, sizeof(title), "worker process %d", i);
            setproctitle(title);

            workerMain(i);
            exit(0);
        }
    }

    /* Master monitoring loop */
    while (1) {
        wait(NULL);
        /* Respawn workers if needed */
    }

    return 0;
}
```

**Terminal output:**

```bash
$ ps aux | grep myserver
user  10000  master process
user  10001  worker process 0
user  10002  worker process 1
user  10003  worker process 2
user  10004  worker process 3
```

### Example 5: State Machine Visualization

```c
typedef enum ServerState {
    STATE_INIT,
    STATE_LOADING_CONFIG,
    STATE_CONNECTING_DB,
    STATE_READY,
    STATE_MAINTENANCE,
    STATE_SHUTDOWN
} ServerState;

void setServerState(ServerState state) {
    const char *stateNames[] = {
        "initializing",
        "loading configuration",
        "connecting to database",
        "ready",
        "maintenance mode",
        "shutting down"
    };

    char title[128];
    snprintf(title, sizeof(title), "server: %s", stateNames[state]);
    setproctitle(title);
}

int main(int argc, char *argv[]) {
    setProctitleInit(argv);

    setServerState(STATE_INIT);

    setServerState(STATE_LOADING_CONFIG);
    if (!loadConfig()) {
        return 1;
    }

    setServerState(STATE_CONNECTING_DB);
    if (!connectDatabase()) {
        return 1;
    }

    setServerState(STATE_READY);
    serverLoop();

    setServerState(STATE_SHUTDOWN);
    cleanup();

    return 0;
}
```

## Implementation Details

### Memory Layout (Linux/macOS)

When a program starts, `argv[]` and `environ[]` are stored contiguously in memory:

```
[argv[0]][argv[1]]...[argv[n]][NULL][environ[0]][environ[1]]...[environ[m]][NULL]
```

**Initialization Process:**

1. Allocate a buffer sized for the whole environment
2. Follow the contiguous run of strings starting at `argv[0]` through `argv[]`
   and into `environ[]`, tracking the last byte of that run
3. Copy each environment string inside the run into the new buffer and repoint
   `environ[i]` at the copy
4. Set `argv[1] = NULL`
5. The original region up to the tracked last byte is now available for titles

**Title Setting Process:**

1. Copy the title over `argv[0]`, bounded by the tracked last byte
2. Pad the remaining space with `\0`

### Thread Safety

- `setProctitleInit()` must be called once before any threads are created
- The module keeps two file-scope pointers (the saved `argv` and the region end)
  written only by init
- `setproctitle()` does no locking, so concurrent callers race on the argv
  region; serialize it externally if more than one thread sets the title

### Performance

- **Initialization**: O(n) where n = total environment variable size
- **Setting Title**: O(m) where m = title length
- **Memory Overhead**: One allocation sized for the environment (typically < 1KB),
  never freed - the relocated environment must stay live for the process lifetime

## Platform Support

| Platform | Support | Method            | Init call                      |
| -------- | ------- | ----------------- | ------------------------------ |
| FreeBSD  | Native  | libc setproctitle | `setProctitleInit()` - no args |
| NetBSD   | Native  | libc setproctitle | `setProctitleInit()` - no args |
| OpenBSD  | Native  | libc setproctitle | `setProctitleInit()` - no args |
| Linux    | Custom  | argv relocation   | `setProctitleInit(argv)`       |
| macOS    | Custom  | argv relocation   | `setProctitleInit(argv)`       |
| Others   | No-op   | Empty macros      | `setProctitleInit()` - no args |

There is no Windows support in datakit.

## Best Practices

### 1. Parse Arguments First, Then Initialize

Init nulls `argv[1]` and takes over the argv region, so argument parsing must
already be done (or the strings copied) by the time it runs. It must also run
before anything else touches `argv` or `environ`.

```c
/* GOOD - parse first, then hand the region over */
int main(int argc, char *argv[]) {
    Config cfg = parseArgs(argc, argv);  /* copies what it needs */
    setProctitleInit(argv);
    /* ... rest of program ... */
}

/* BAD - argv[1] is already NULL here */
int main(int argc, char *argv[]) {
    setProctitleInit(argv);
    Config cfg = parseArgs(argc, argv);  /* reads NULL at index 1 */
}
```

### 2. Keep Titles Short and Descriptive

```c
/* GOOD - Concise and informative */
setproctitle("worker 3: processing");
setproctitle("server: 42 connections");

/* BAD - Too verbose */
setproctitle("worker process number 3 is currently processing job ID 12345 from queue 'high-priority' with priority level 9");

/* BAD - Not informative */
setproctitle("running");
```

### 3. Update Titles on State Changes

```c
/* GOOD - Shows current state */
setproctitle("server: starting");
initialize();
setproctitle("server: ready");
while (running) {
    /* ... */
}
setproctitle("server: stopping");

/* BAD - Set once and forget */
setproctitle("server");
/* Title never updated */
```

### 4. Don't Branch on the Init Return Value

Every variant returns true: the Linux/macOS implementation has no failure path
(allocation failure is fatal in datakit), and the BSD and fallback variants are
macros that literally expand to `true`. Testing the result is dead code.

```c
/* FINE - just call it */
setProctitleInit(argv);

/* POINTLESS - the false branch is unreachable */
if (!setProctitleInit(argv)) {
    fprintf(stderr, "Failed to initialize process title\n");
    return 1;
}
```

### 5. Guard the Init Call, Not the Title Call

```c
/* GOOD - the init signature differs per platform */
#if DK_OS_LINUX || DK_OS_APPLE
    setProctitleInit(argv);
#else
    setProctitleInit();
#endif

/* setproctitle() itself is uniform - no guard needed */
setproctitle("server: ready");
```

## Debugging

### Title Not Visible

**Problem**: Title doesn't change in `ps` output

**Solutions**:

1. Ensure `setProctitleInit()` was called (Linux/macOS)
2. Check that the title fits: it is truncated to the reclaimed argv region, so a
   short original command line means a short maximum title
3. Confirm you are not on a platform where `setproctitle` compiles to nothing
4. Some process viewers cache titles - restart them

### Compile Error at the Init Call

**Problem**: "too many arguments provided to function-like macro invocation"

**Cause**: You called `setProctitleInit(argv)` on a BSD or fallback platform,
where it is a zero-argument macro. Guard it - see
[Guard the Init Call](#5-guard-the-init-call-not-the-title-call).

### Crash Inside setproctitle()

**Problem**: assertion failure, or a NULL dereference with assertions disabled

**Cause**: `setproctitle()` was called before `setProctitleInit()` on Linux/macOS.
The saved `argv` pointer and region end are still NULL.

## Use Cases

1. **Long-Running Servers**: Show connection count, state, or current operation
2. **Worker Pools**: Identify worker ID and current task
3. **Daemons**: Show daemon state (starting, running, stopping)
4. **Multi-Process Applications**: Distinguish master from workers
5. **Debugging**: Identify which process is doing what
6. **Monitoring**: Quick status check without examining logs
7. **Production Diagnostics**: Identify hung or busy processes

## See Also

- BSD `setproctitle(3)` man page
- nginx setproctitle implementation
- Linux `/proc/[pid]/cmdline` documentation

## Testing

The setproctitle module is tested through practical usage in daemon applications. Test by:

1. Calling functions and verifying with `ps aux | grep process-name`
2. Checking different platforms (BSD vs Linux)
3. Testing title updates during runtime
4. Verifying initialization with various argv lengths

**Manual Test:**

```c
int main(int argc, char *argv[]) {
    setProctitleInit(argv);

    printf("Check 'ps' now - should see original command\n");
    sleep(5);

    setproctitle("test: changed title");
    printf("Check 'ps' now - should see 'test: changed title'\n");
    sleep(5);

    return 0;
}
```

Run in terminal:

```bash
# Terminal 1
$ ./test-program --some-args

# Terminal 2 (during first sleep)
$ ps aux | grep test-program
user  12345  ./test-program --some-args

# Terminal 2 (during second sleep)
$ ps aux | grep test-program
user  12345  test: changed title
```
