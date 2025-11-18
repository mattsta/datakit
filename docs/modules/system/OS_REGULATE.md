# OSRegulate - OS Resource Management and Limits

## Overview

`OSRegulate` provides **cross-platform OS resource management and system queries**. It handles process daemonization, resource limits, CPU detection, memory monitoring, network validation, and system configuration checks.

**Key Features:**
- Process daemonization and management
- File descriptor limit adjustment
- CPU count detection
- Memory usage and configuration checks
- Network address validation (private/public detection)
- TCP Fast Open and backlog configuration
- Resource usage statistics (CPU time, RSS, etc.)
- Linux-specific memory metrics (overcommit, THP, smap)

**Header**: `OSRegulate.h`

**Source**: `OSRegulate.c`

**Platforms**: Linux, macOS, FreeBSD, others (with feature detection)

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
3. **Redirect stdio** - Sets stdin/stdout/stderr to /dev/null
4. **Returns** - Parent gets PARENT status, child gets CHILD status

### PID File Management

```c
/* Write PID to file and lock it
 * path: file path to write PID
 * Returns: true on success, false if already locked (another instance running)
 */
bool OSRegulateWritePidToFile(char *path);

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
 */
bool OSRegulateParentStillExisits(void);

/* Example: Child monitors parent */
if (OSRegulateRequestSignalChildWhenParentExits(SIGTERM)) {
    printf("Will receive SIGTERM if parent exits\n");
}

while (running) {
    if (!OSRegulateParentStillExisits()) {
        printf("Parent died, exiting\n");
        break;
    }
    // ... do work ...
}
```

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

/* Example: Check Redis-style memory settings */
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

#ifdef __linux__
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

| Feature | Linux | macOS | FreeBSD | Other |
|---------|-------|-------|---------|-------|
| Daemonize | ✓ | ✓ | ✓ | ✓ |
| PID file | ✓ | ✓ | ✓ | ✓ |
| Parent death signal | ✓ | ✗ | ✗ | ✗ |
| CPU count | ✓ | ✓ | ✓ | ✓ |
| FD limit adjust | ✓ | ✓ | ✓ | ✓ |
| TFO mode | ✓ | ✗ | ✗ | ✗ |
| TCP backlog check | ✓ | ✗ | ✗ | ✗ |
| Total memory | ✓ | ✓ | ✓ | ✗ |
| RSS | ✓ | ✓ | ✗ | ✗ |
| Overcommit check | ✓ | ✗ | ✗ | ✗ |
| THP check | ✓ | ✗ | ✗ | ✗ |
| SMAP parsing | ✓ | ✗ | ✗ | ✗ |
| Resource usage | ✓ | ✓ | ✓ | ✓ |
| Network validation | ✓ | ✓ | ✓ | ✓ |

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

### 2. Use Platform Guards

```c
/* GOOD */
#ifdef __linux__
    if (!OSRegulateLinuxOvercommitEnabled()) {
        // Handle Linux-specific setting...
    }
#endif

/* BAD */
if (!OSRegulateLinuxOvercommitEnabled()) {
    // Fails to compile on non-Linux!
}
```

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

- System documentation for resource limits (`man setrlimit`)
- Linux proc filesystem (`man proc`)
- TCP Fast Open (`man tcp`, search for TCP_FASTOPEN)

## Testing

The OSRegulate module includes validation in server/daemon applications but doesn't have a dedicated test suite. Integration testing is done through actual daemon usage.
