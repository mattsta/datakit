# OSRegulate - OS Resource Management and Limits

## Overview

`OSRegulate` provides **POSIX OS resource management and system queries**. It handles process daemonization, resource limits, CPU detection, memory monitoring, network validation, and system configuration checks.

**Key Features:**

- Process daemonization and management
- Parent-death detection (`PR_SET_PDEATHSIG` on Linux, kqueue on macOS/BSD)
- File descriptor limit adjustment
- CPU count detection
- Memory usage and configuration checks
- Network address validation (private/wildcard detection)
- TCP Fast Open and backlog configuration
- Resource usage statistics (CPU time, RSS, etc.)
- Linux-specific memory metrics (overcommit, THP, smaps)

**Header**: `OSRegulate.h`

**Source**: `OSRegulate.c`

**Platforms**: Linux, macOS, and the BSD family (FreeBSD, NetBSD, OpenBSD,
DragonFly), with per-feature detection. Some entry points are compiled only on
Linux - see [Platform Support](#platform-support).

## Process Management

### Daemonization

```c
/* Status returned from daemonize operations */
typedef enum OSRegulateDaemonizeStatus {
    OS_REGULATE_DAEMONIZE_PARENT,  /* Caller is parent process */
    OS_REGULATE_DAEMONIZE_CHILD,   /* Caller is child (daemon) */
    OS_REGULATE_DAEMONIZE_ERROR    /* Error occurred */
} OSRegulateDaemonizeStatus;

/* Daemonize current process
 * errMsg: optional pointer to receive error message
 * Returns: PARENT, CHILD, or ERROR
 */
OSRegulateDaemonizeStatus OSRegulateDaemonize(const char **errMsg);

/* Daemonize and exit parent after callback returns true
 * cb: callback to check if child is ready
 * userData: passed to callback
 */
void OSRegulateDaemonizeThenExit(
    OSRegulateDaemonizeParentExitCallback *cb,
    void *userData
);

/* Daemonize and exit parent immediately */
void OSRegulateDaemonizeThenExitNoWait(void);

/* Exit process cleanly (calls _exit) */
void OSRegulateExitClean(void);

/* Example: Basic daemonization */
const char *error = NULL;
OSRegulateDaemonizeStatus status = OSRegulateDaemonize(&error);

switch (status) {
case OS_REGULATE_DAEMONIZE_PARENT:
    printf("Parent process exiting\n");
    exit(0);

case OS_REGULATE_DAEMONIZE_CHILD:
    printf("Running as daemon\n");
    // Continue daemon work...
    break;

case OS_REGULATE_DAEMONIZE_ERROR:
    fprintf(stderr, "Daemonize failed: %s\n", error);
    exit(1);
}
```

#### What Daemonization Does

1. **Fork** - Creates child process
2. **setsid()** - Creates new session (detaches from terminal)
3. **Redirect stdio** - Points stdin and stdout at `/dev/null`.
   **stderr is deliberately left connected**, so daemon error output still
   reaches whatever the process was launched with; redirect it yourself if you
   want it silenced.
4. **Returns** - Parent gets PARENT status, child gets CHILD status

Every failing step returns `OS_REGULATE_DAEMONIZE_ERROR` and, when `errMsg` is
non-NULL, sets it to a static description (`"Failure to fork() daemon"`,
`"Failed to run setsid()"`, `"Failed to open /dev/null"`, ...). The string is not
owned by the caller and must not be freed.

### PID File Management

```c
/* Write PID to file and lock it
 * path: file path to write PID
 * Returns: true on success, false if already locked (another instance running)
 */
bool OSRegulateWritePidToFile(const char *path);

/* Example: Ensure single instance */
if (!OSRegulateWritePidToFile("/var/run/myapp.pid")) {
    fprintf(stderr, "Another instance already running\n");
    exit(1);
}

/* PID file remains locked while process runs */
```

### Parent Process Monitoring

```c
/* Request signal when parent process exits (Linux only)
 * sig: signal to receive (e.g., SIGTERM, SIGKILL)
 * Returns: true on success, false if unsupported
 */
bool OSRegulateRequestSignalChildWhenParentExits(int sig);

/* Check if parent process still exists
 * Returns: true if parent is not init (PID 1), false if reparented
 *
 * Note the spelling: "Exisits". This is a getppid() poll, so it only
 * notices parent death the next time you call it.
 */
bool OSRegulateParentStillExisits(void);

/* Parent-death watch fd (kqueue platforms: macOS/BSD)
 * parent: PID to watch
 * Returns: a kqueue fd armed with a one-shot EVFILT_PROC/NOTE_EXIT filter
 *          on `parent`, or -1 on Linux (PR_SET_PDEATHSIG covers that case)
 *          or on error
 *
 * Block in kevent() on the returned fd to receive an event the instant the
 * parent process exits - no polling, and no dependence on the health of a
 * data-plane thread. On -1 the caller should fall back to polling
 * OSRegulateParentStillExisits(). Close the fd with close() when done.
 */
int OSRegulateParentDeathKqueueFd(pid_t parent);

/* Example: Child monitors parent */
if (OSRegulateRequestSignalChildWhenParentExits(SIGTERM)) {
    /* Linux: kernel signals us directly, nothing else to do */
    printf("Will receive SIGTERM if parent exits\n");
} else {
    /* macOS/BSD: block a dedicated thread on the kqueue fd */
    const int kq = OSRegulateParentDeathKqueueFd(getppid());
    if (kq >= 0) {
        struct kevent ev;
        (void)kevent(kq, NULL, 0, &ev, 1, NULL); /* blocks until parent exits */
        printf("Parent died, exiting\n");
        close(kq);
    } else {
        /* Last resort: poll */
        while (running && OSRegulateParentStillExisits()) {
            // ... do work ...
        }
    }
}
```

**Choosing a parent-death mechanism:**

| Platform  | Mechanism                                  | API                                           |
| --------- | ------------------------------------------ | --------------------------------------------- |
| Linux     | `prctl(PR_SET_PDEATHSIG)` - kernel signal  | `OSRegulateRequestSignalChildWhenParentExits` |
| macOS/BSD | kqueue `EVFILT_PROC` / `NOTE_EXIT` - event | `OSRegulateParentDeathKqueueFd`               |
| Fallback  | `getppid()` poll                           | `OSRegulateParentStillExisits`                |

`OSRegulateRequestSignalChildWhenParentExits()` returns false on every non-Linux
platform, which is the signal to reach for the kqueue fd instead.

## CPU Information

```c
/* Get number of online CPUs
 * Returns: CPU count (at least 1)
 */
size_t OSRegulateCPUCountGet(void);

/* Example: Set worker thread count */
size_t cpus = OSRegulateCPUCountGet();
size_t workers = cpus * 2; // 2 workers per CPU

printf("Detected %zu CPUs, starting %zu workers\n", cpus, workers);
```

## File Descriptor Limits

```c
/* Adjust open file limit
 * requestedFdCount: desired number of file descriptors
 * limitActuallySet: output for actual limit set (can be NULL)
 * statusMsg: output for status message (can be NULL)
 * Returns: true if limit >= requested, false otherwise
 */
bool OSRegulateAdjustOpenFilesLimit(
    const size_t requestedFdCount,
    size_t *limitActuallySet,
    char **statusMsg
);

/* Example: Increase FD limit for server */
size_t actualLimit;
char *msg;

if (OSRegulateAdjustOpenFilesLimit(10000, &actualLimit, &msg)) {
    printf("Success: %s\n", msg);
} else {
    fprintf(stderr, "Warning: %s\n", msg);
    printf("Using limit of %zu instead of 10000\n", actualLimit);
}

/* Sample output:
   "Increased maximum number of open files to 10064 (was originally 1024)."
*/
```

### How Limit Adjustment Works

1. **Get current limit** via `getrlimit(RLIMIT_NOFILE)`
2. **If already sufficient** - Return success
3. **If too low** - Try `setrlimit()` with requested + overhead
4. **If setrlimit fails** - Retry with smaller values (decrement by 16)
5. **Return best achievable** limit

## Network Configuration

### TCP Fast Open

```c
/* Get TCP Fast Open mode
 * Returns: 0 (disabled), 1 (client), 2 (server), 3 (both)
 * Reads from: /proc/sys/net/ipv4/tcp_fastopen (Linux)
 *
 * Gated on the running kernel version via versionOSRuntime's
 * linuxKernelHasTFOServerIPv4 (>= 3.7). Returns 0 anywhere the file
 * cannot be read, including all non-Linux platforms.
 */
int OSRegulateTFOMode(void);

/* Example */
int tfoMode = OSRegulateTFOMode();
if (tfoMode & 2) {
    printf("TFO enabled for servers\n");
    // Enable TFO socket option...
}
```

### TCP Backlog Limit

```c
/* Check if TCP backlog limit is sufficient
 * tcpBacklogListenLength: desired listen() backlog
 * Returns: true if system limit >= requested
 * Reads from: /proc/sys/net/core/somaxconn (Linux)
 *
 * Where /proc/sys/net/core/somaxconn is unavailable there is nothing to
 * check, so this returns true ("no known problem"), not false.
 */
bool OSRegulateTcpBacklogMeetsLimit(int tcpBacklogListenLength);

/* Example */
int desiredBacklog = 512;

if (!OSRegulateTcpBacklogMeetsLimit(desiredBacklog)) {
    fprintf(stderr, "WARNING: System somaxconn < %d\n", desiredBacklog);
    fprintf(stderr, "Consider: echo 512 > /proc/sys/net/core/somaxconn\n");
}
```

## Memory Information

### Total and Current Memory

```c
/* Get total physical RAM in bytes
 * Returns: total memory or 0 on error
 */
size_t OSRegulateTotalMemoryGet(void);

/* Get current RSS (Resident Set Size) in bytes
 * Returns: current RSS or 0 on error
 */
size_t OSRegulateRSSGet(void);

/* Example */
size_t totalMem = OSRegulateTotalMemoryGet();
size_t currentRSS = OSRegulateRSSGet();

printf("System memory: %zu MB\n", totalMem / 1024 / 1024);
printf("Process RSS: %zu MB (%.1f%% of total)\n",
       currentRSS / 1024 / 1024,
       (double)currentRSS / totalMem * 100);
```

### Linux Memory Configuration

> **Linux-only symbols.** Everything in this section and the next
> (`OSRegulateLinuxOvercommitEnabled`, `OSRegulateLinuxTransparentHugePagesEnabled`,
> `OSRegulateLinuxMemorySettingsAreOkay`, `OSRegulateLinuxSmapBytesByFieldForPid`,
> `OSRegulateLinuxTransparentHugePagesGetAnonHugePagesSize`,
> `OSRegualteLinuxSmapPrivateDirtyGet`) is compiled only on Linux. The header
> declares them unconditionally, so calling them elsewhere compiles but fails to
> link. Guard the call sites with `#if DK_OS_LINUX`.

```c
/* Check if memory overcommit is enabled
 * Returns: true if overcommit enabled (settings 0 or 1)
 * Reads from: /proc/sys/vm/overcommit_memory
 */
bool OSRegulateLinuxOvercommitEnabled(void);

/* Check if Transparent Huge Pages enabled
 * Returns: true if THP enabled (not set to [never])
 * Reads from: /sys/kernel/mm/transparent_hugepage/enabled
 */
bool OSRegulateLinuxTransparentHugePagesEnabled(void);

/* Check if memory settings are optimal
 * Returns: true if overcommit enabled AND THP disabled
 */
bool OSRegulateLinuxMemorySettingsAreOkay(void);

/* Example: Check memory settings */
if (!OSRegulateLinuxMemorySettingsAreOkay()) {
    if (!OSRegulateLinuxOvercommitEnabled()) {
        fprintf(stderr, "WARNING: overcommit_memory is disabled\n");
        fprintf(stderr, "Recommend: echo 1 > /proc/sys/vm/overcommit_memory\n");
    }

    if (OSRegulateLinuxTransparentHugePagesEnabled()) {
        fprintf(stderr, "WARNING: Transparent Huge Pages is enabled\n");
        fprintf(stderr, "Recommend: echo never > "
                       "/sys/kernel/mm/transparent_hugepage/enabled\n");
    }
}
```

### Linux SMAP Analysis

```c
/* Get sum of specific field from /proc/[pid]/smaps
 * field: field name with trailing ":" (e.g., "Rss:", "Private_Dirty:")
 * pid: process ID, or -1 for self
 * Returns: total bytes for field across all mappings
 */
size_t OSRegulateLinuxSmapBytesByFieldForPid(
    const char *field,
    int64_t pid
);

/* Get anonymous huge pages size
 * pid: process ID, or -1 for self
 * Returns: total AnonHugePages in bytes
 *
 * Convenience wrapper over the "AnonHugePages:" field. Note the int
 * return type: totals above INT_MAX (2 GB of huge pages) truncate. Call
 * OSRegulateLinuxSmapBytesByFieldForPid("AnonHugePages:", pid) directly
 * for a full-width size_t result.
 */
int OSRegulateLinuxTransparentHugePagesGetAnonHugePagesSize(
    int64_t pid
);

/* Get private dirty memory
 * pid: process ID, or -1 for self
 * Returns: total Private_Dirty in bytes
 */
size_t OSRegualteLinuxSmapPrivateDirtyGet(int64_t pid);

/* Example: Analyze memory in detail */
size_t rss = OSRegulateLinuxSmapBytesByFieldForPid("Rss:", -1);
size_t privateDirty = OSRegualteLinuxSmapPrivateDirtyGet(-1);
size_t anonHuge = OSRegulateLinuxTransparentHugePagesGetAnonHugePagesSize(-1);

printf("RSS: %zu MB\n", rss / 1024 / 1024);
printf("Private Dirty: %zu MB\n", privateDirty / 1024 / 1024);
printf("Anon Huge Pages: %zu MB\n", anonHuge / 1024 / 1024);
```

## Resource Usage Statistics

```c
/* Get resource usage statistics
 * selfMaxRSS: output for self max RSS in bytes
 * childMaxRSS: output for children max RSS in bytes
 * selfSystemCPU: output for self system CPU seconds
 * selfUserCPU: output for self user CPU seconds
 * childSystemCPU: output for children system CPU seconds
 * childUserCPU: output for children user CPU seconds
 * Returns: true on success
 */
bool OSRegulateResourceUsageGet(
    long *selfMaxRSS,
    long *childMaxRSS,
    float *selfSystemCPU,
    float *selfUserCPU,
    float *childSystemCPU,
    float *childUserCPU
);

/* Example: Print resource usage */
long selfRSS, childRSS;
float selfSysCPU, selfUserCPU, childSysCPU, childUserCPU;

if (OSRegulateResourceUsageGet(&selfRSS, &childRSS,
                               &selfSysCPU, &selfUserCPU,
                               &childSysCPU, &childUserCPU)) {
    printf("Self:\n");
    printf("  Max RSS: %ld MB\n", selfRSS / 1024 / 1024);
    printf("  System CPU: %.2f seconds\n", selfSysCPU);
    printf("  User CPU: %.2f seconds\n", selfUserCPU);
    printf("  Total CPU: %.2f seconds\n", selfSysCPU + selfUserCPU);

    if (childRSS > 0 || childSysCPU > 0 || childUserCPU > 0) {
        printf("Children:\n");
        printf("  Max RSS: %ld MB\n", childRSS / 1024 / 1024);
        printf("  System CPU: %.2f seconds\n", childSysCPU);
        printf("  User CPU: %.2f seconds\n", childUserCPU);
    }
}
```

## Network Address Validation

### Private Address Detection

```c
/* Check if address is private (RFC 1918, RFC 4193, loopback, link-local)
 * ai: addrinfo structure to check
 * Returns: true if private/local address
 */
bool OSRegulateNetworkIsPrivate(const struct addrinfo *ai);

/* Check if IPv4 address is private */
bool OSRegulateNetworkIsPrivateIPv4(const struct sockaddr_in *sa);

/* Check if IPv6 address is private */
bool OSRegulateNetworkIsPrivateIPv6(const struct sockaddr_in6 *sa6);

/* Example: Validate bind address */
struct addrinfo *ai = /* ... getaddrinfo() ... */;

if (OSRegulateNetworkIsPrivate(ai)) {
    printf("Binding to private address - local only\n");
} else {
    printf("WARNING: Binding to public address!\n");
}
```

**Private IPv4 Ranges Detected:**

- `127.0.0.0/8` - Loopback
- `192.168.0.0/16` - Private
- `172.16.0.0/12` - Private
- `10.0.0.0/8` - Private

**Private IPv6 Ranges Detected:**

- `::1/128` - Loopback
- `fc00::/7` - Unique Local (ULA)
- `fe80::/10` - Link-Local

### Wildcard Address Detection

```c
/* Check if address is all zeros (0.0.0.0 or ::)
 * ai: addrinfo structure to check
 * Returns: true if wildcard address
 */
bool OSRegulateNetworkIsAll(const struct addrinfo *ai);

/* Check if IPv4 address is 0.0.0.0 */
bool OSRegulateNetworkIsAllIPv4(const struct sockaddr_in *sa);

/* Check if IPv6 address is :: */
bool OSRegulateNetworkIsAllIPv6(const struct sockaddr_in6 *sa6);

/* Example */
if (OSRegulateNetworkIsAll(ai)) {
    printf("Binding to all interfaces\n");
}
```

## Real-World Examples

### Example 1: Server Initialization

```c
bool serverInit(int requestedConnections) {
    printf("Initializing server...\n");

    /* Check CPU count */
    size_t cpus = OSRegulateCPUCountGet();
    printf("CPUs: %zu\n", cpus);

    /* Adjust file descriptor limit */
    size_t fdLimit;
    char *msg;
    if (!OSRegulateAdjustOpenFilesLimit(requestedConnections, &fdLimit, &msg)) {
        fprintf(stderr, "FD limit warning: %s\n", msg);
        if (fdLimit < requestedConnections) {
            fprintf(stderr, "Cannot support %d connections\n",
                   requestedConnections);
            return false;
        }
    } else {
        printf("FD limit: %s\n", msg);
    }

    /* Check TCP settings */
    if (!OSRegulateTcpBacklogMeetsLimit(512)) {
        fprintf(stderr, "WARNING: TCP backlog limit too low\n");
    }

    int tfo = OSRegulateTFOMode();
    if (tfo & 2) {
        printf("TCP Fast Open enabled for servers\n");
    }

    /* Check memory settings (Linux) */
    if (!OSRegulateLinuxMemorySettingsAreOkay()) {
        fprintf(stderr, "WARNING: Suboptimal memory settings\n");
    }

    printf("Server initialization complete\n");
    return true;
}
```

### Example 2: Daemon with Parent Monitoring

```c
int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--daemon") == 0) {
        /* Daemonize */
        if (OSRegulateDaemonize(NULL) == OS_REGULATE_DAEMONIZE_PARENT) {
            /* Parent exits after writing PID */
            if (!OSRegulateWritePidToFile("/var/run/myapp.pid")) {
                fprintf(stderr, "Already running\n");
                return 1;
            }
            printf("Daemon started\n");
            OSRegulateExitClean();
        }

        /* Child continues */
        printf("Running as daemon (PID %d)\n", getpid());
    } else {
        /* Foreground mode - monitor parent */
        OSRegulateRequestSignalChildWhenParentExits(SIGTERM);
    }

    /* Main loop */
    while (running) {
        if (!OSRegulateParentStillExisits()) {
            printf("Parent died, shutting down\n");
            break;
        }

        // ... do work ...
    }

    return 0;
}
```

### Example 3: Memory Monitoring

```c
void printMemoryStatus(void) {
    size_t totalMem = OSRegulateTotalMemoryGet();
    size_t currentRSS = OSRegulateRSSGet();

    printf("=== Memory Status ===\n");
    printf("Total system: %zu MB\n", totalMem / 1024 / 1024);
    printf("Process RSS: %zu MB (%.1f%%)\n",
           currentRSS / 1024 / 1024,
           (double)currentRSS / totalMem * 100);

#if DK_OS_LINUX
    size_t privateDirty = OSRegualteLinuxSmapPrivateDirtyGet(-1);
    size_t anonHuge = OSRegulateLinuxTransparentHugePagesGetAnonHugePagesSize(-1);

    printf("Private dirty: %zu MB\n", privateDirty / 1024 / 1024);
    printf("Anon huge pages: %zu MB\n", anonHuge / 1024 / 1024);

    if (!OSRegulateLinuxMemorySettingsAreOkay()) {
        printf("WARNING: Memory settings not optimal\n");
    }
#endif

    /* Get resource usage */
    long maxRSS, childRSS;
    float sysCPU, userCPU, childSysCPU, childUserCPU;

    if (OSRegulateResourceUsageGet(&maxRSS, &childRSS,
                                   &sysCPU, &userCPU,
                                   &childSysCPU, &childUserCPU)) {
        printf("Max RSS: %ld MB\n", maxRSS / 1024 / 1024);
        printf("CPU time: %.2fs system, %.2fs user\n", sysCPU, userCPU);
    }
}
```

### Example 4: Network Binding Validation

```c
bool bindToAddress(const char *host, const char *port) {
    struct addrinfo hints = {0}, *result, *rp;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(host, port, &hints, &result) != 0) {
        return false;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        /* Check address type */
        if (OSRegulateNetworkIsAll(rp)) {
            printf("Binding to all interfaces\n");
        } else if (OSRegulateNetworkIsPrivate(rp)) {
            printf("Binding to private address (local only)\n");
        } else {
            printf("WARNING: Binding to public address\n");
        }

        /* Create and bind socket */
        int sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == -1) continue;

        if (bind(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            printf("Successfully bound to address\n");
            freeaddrinfo(result);
            return true;
        }

        close(sock);
    }

    freeaddrinfo(result);
    return false;
}
```

## Platform Support

Supported platforms are Linux, macOS, and the BSD family. There is no Windows
support anywhere in datakit.

| Feature                     | Linux | macOS | BSD family | Behavior elsewhere              |
| --------------------------- | ----- | ----- | ---------- | ------------------------------- |
| Daemonize                   | ✓     | ✓     | ✓          | POSIX fork/setsid               |
| PID file                    | ✓     | ✓     | ✓          | POSIX flock                     |
| Parent death signal (prctl) | ✓     | ✗     | ✗          | returns false                   |
| Parent death kqueue fd      | ✗     | ✓     | ✓          | returns -1                      |
| Parent poll (getppid)       | ✓     | ✓     | ✓          | always available                |
| CPU count                   | ✓     | ✓     | ✓          | `_SC_NPROCESSORS_ONLN`          |
| FD limit adjust             | ✓     | ✓     | ✓          | POSIX get/setrlimit             |
| TFO mode                    | ✓     | ✗     | ✗          | returns 0                       |
| TCP backlog check           | ✓     | ✗     | ✗          | returns true (nothing to check) |
| Total memory                | ✓     | ✓     | ✓          | sysctl / sysconf, 0 if neither  |
| RSS                         | ✓     | ✓     | partial    | 0 where unsupported             |
| Overcommit / THP / smaps    | ✓     | ✗     | ✗          | **not compiled** - link error   |
| Resource usage              | ✓     | ✓     | ✓          | POSIX getrusage                 |
| Network validation          | ✓     | ✓     | ✓          | pure address math               |

The "kqueue" rows cover macOS, FreeBSD, NetBSD, OpenBSD, and DragonFly.
`OSRegulateRSSGet()` uses `/proc/self/stat` on Linux and Mach task info on macOS;
elsewhere it returns 0.

## Best Practices

### 1. Check Return Values

```c
/* GOOD */
if (!OSRegulateAdjustOpenFilesLimit(10000, NULL, NULL)) {
    fprintf(stderr, "Failed to set FD limit\n");
    // Handle gracefully...
}

/* BAD */
OSRegulateAdjustOpenFilesLimit(10000, NULL, NULL);
// No error handling!
```

### 2. Use Platform Guards for the Linux-Only Functions

The `OSRegulate*Linux*` / `OSRegualte*Linux*` entry points are compiled only on
Linux. Because the header declares them unconditionally, an unguarded call
compiles fine and then fails at **link** time on macOS/BSD.

```c
/* GOOD */
#if DK_OS_LINUX
    if (!OSRegulateLinuxOvercommitEnabled()) {
        // Handle Linux-specific setting...
    }
#endif

/* BAD */
if (!OSRegulateLinuxOvercommitEnabled()) {
    // Undefined symbol at link time on non-Linux!
}
```

Everything else is portable: functions with no implementation on a platform
return a documented neutral value (false, 0, -1, or true for the backlog check)
rather than failing to link.

### 3. Provide Fallbacks

```c
/* GOOD */
size_t cpus = OSRegulateCPUCountGet();
if (cpus == 0) {
    cpus = 1;  /* Fallback */
}

/* GOOD */
size_t totalMem = OSRegulateTotalMemoryGet();
if (totalMem == 0) {
    totalMem = 1024 * 1024 * 1024;  /* Assume 1GB */
}
```

## See Also

- [versionOSRuntime](VERSION_OS_RUNTIME.md) - kernel version gates used by
  `OSRegulateTFOMode()`
- System documentation for resource limits (`man setrlimit`)
- Linux proc filesystem (`man proc`), `man 2 prctl` for `PR_SET_PDEATHSIG`
- macOS/BSD event notification (`man 2 kqueue`, `EVFILT_PROC` / `NOTE_EXIT`)
- TCP Fast Open (`man tcp`, search for TCP_FASTOPEN)

## Testing

The OSRegulate module includes validation in server/daemon applications but doesn't have a dedicated test suite. Integration testing is done through actual daemon usage.
