# SetProcTitle - Process Title Manipulation

## Overview

`SetProcTitle` provides **cross-platform process title manipulation** for Unix-like systems. It allows changing the process name visible in system tools like `ps`, `top`, and `htop`, which is useful for identifying server states, worker roles, or debugging.

**Key Features:**

- Cross-platform process title modification
- Automatic memory management for title storage
- Native BSD support with fallback for Linux/macOS/Solaris
- Thread-safe initialization
- Zero overhead on platforms with native support

**Header**: `setproctitle.h`

**Source**: `setproctitle.c`

**Platforms**: FreeBSD, NetBSD, OpenBSD (native), Linux, macOS, Solaris (custom implementation)

**Origin**: Modified from nginx's setproctitle implementation (BSD-2 license)

## API Reference

### Initialization

```c
/* Initialize process title infrastructure
 * argv: original argv from main()
 * Returns: true on success, false on error
 *
 * Must be called before setproctitle() on Linux/macOS/Solaris
 * No-op on BSD platforms (native support)
 */
bool setProctitleInit(char *argv[]);
```

**Important**: Must be called early in `main()` before `argv` or `environ` are modified.

### Setting Process Title

```c
/* Set process title
 * title: new process title string
 *
 * On BSD platforms: formats as "progname: title"
 * On Linux/macOS: replaces argv[0] with title
 * On Solaris: appends original command in parentheses if needed
 */
void setproctitle(char *title);
```

## Platform Differences

### BSD Platforms (FreeBSD, NetBSD, OpenBSD)

BSD systems provide native `setproctitle()` support:

```c
/* On BSD, setProctitleInit() is a no-op macro */
#define setProctitleInit() true

/* Native setproctitle() called with format string */
#define setproctitle(title) setproctitle("%s", title)
```

**No initialization required** - the system provides the functionality.

### Linux, macOS, Solaris

These platforms require custom implementation:

```c
bool setProctitleInit(char *argv[]);
void setproctitle(char *title);
```

**How It Works:**

1. **Memory Relocation**: `setProctitleInit()` relocates `environ[]` to new memory
2. **Space Calculation**: Determines available space from `argv[0]` to last environment variable
3. **Title Storage**: `setproctitle()` writes title into the relocated space
4. **Padding**: Fills remaining space with `\0` (Linux/macOS) or spaces (Solaris)

### Solaris Special Behavior

The standard `/bin/ps` on Solaris doesn't show changed titles. Use `/usr/ucb/ps -w` instead.

If the new title is shorter than the original command line, Solaris appends the original command in parentheses:

```
new-title (original command line arguments)
```

## Usage Examples

### Example 1: Basic Server

```c
#include "setproctitle.h"

int main(int argc, char *argv[]) {
    /* Initialize on program startup */
    if (!setProctitleInit(argv)) {
        fprintf(stderr, "Failed to initialize setproctitle\n");
        return 1;
    }

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

### Memory Layout (Linux/macOS/Solaris)

When a program starts, `argv[]` and `environ[]` are stored contiguously in memory:

```
[argv[0]][argv[1]]...[argv[n]][NULL][environ[0]][environ[1]]...[environ[m]][NULL]
```

**Initialization Process:**

1. Calculate total size needed for environment variables
2. Allocate new memory for environment
3. Copy environment strings to new memory
4. Update `environ[]` pointers to new memory
5. Mark entire original region as available for title

**Title Setting Process:**

1. Copy title to `argv[0]` location
2. Set `argv[1] = NULL`
3. Pad remaining space with `\0` or spaces

### Thread Safety

- `setProctitleInit()` must be called once before any threads are created
- `setproctitle()` is safe to call from multiple threads after initialization
- No internal locking required (single write to argv region)

### Performance

- **Initialization**: O(n) where n = total environment variable size
- **Setting Title**: O(m) where m = title length
- **Memory Overhead**: Size of environment variables (typically < 1KB)

## Platform Support

| Platform | Support | Method            | Notes                             |
| -------- | ------- | ----------------- | --------------------------------- |
| FreeBSD  | Native  | System call       | No initialization needed          |
| NetBSD   | Native  | System call       | No initialization needed          |
| OpenBSD  | Native  | System call       | No initialization needed          |
| Linux    | Custom  | argv manipulation | Requires initialization           |
| macOS    | Custom  | argv manipulation | Requires initialization           |
| Solaris  | Custom  | argv manipulation | Requires `/usr/ucb/ps -w` to view |
| Others   | No-op   | Macros            | Functions compile but do nothing  |

## Best Practices

### 1. Initialize Early

```c
/* GOOD - First thing in main() */
int main(int argc, char *argv[]) {
    setProctitleInit(argv);
    /* ... rest of program ... */
}

/* BAD - After argv might be modified */
int main(int argc, char *argv[]) {
    parseArgs(argv);  /* May modify argv! */
    setProctitleInit(argv);  /* Too late! */
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

### 4. Check Return Value of Init

```c
/* GOOD - Handle initialization failure */
if (!setProctitleInit(argv)) {
    fprintf(stderr, "Warning: setproctitle not available\n");
    /* Continue anyway */
}

/* Also GOOD - Critical error */
if (!setProctitleInit(argv)) {
    fprintf(stderr, "Failed to initialize process title\n");
    return 1;
}
```

### 5. Use Platform Guards for Advanced Features

```c
/* GOOD - Handle Solaris differences */
#if DK_OS_SOLARIS
    /* Solaris needs longer titles to show properly */
    setproctitle("server: ready (listening on port 8080)");
#else
    setproctitle("server: ready");
#endif
```

## Debugging

### Title Not Visible

**Problem**: Title doesn't change in `ps` output

**Solutions**:

1. Ensure `setProctitleInit()` was called
2. On Solaris, use `/usr/ucb/ps -w` instead of `/bin/ps`
3. Check if title is too long (may be truncated)
4. Some process viewers cache titles - restart them

### Initialization Failures

**Problem**: `setProctitleInit()` returns false

**Possible Causes**:

1. Memory allocation failed
2. Platform not supported
3. Called after argv/environ were modified

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
