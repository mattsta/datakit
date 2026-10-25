# VersionOSRuntime - OS and Runtime Version Detection

## Overview

`VersionOSRuntime` provides **automatic OS kernel version detection** at program startup. It captures the running kernel version and provides compile-time macros to check for feature availability based on kernel version requirements.

**Key Features:**

- Automatic initialization via constructor attribute
- Single global version variable; checks are one integer comparison
- Named macros for Linux kernel feature detection
- Support for major.minor.patch version comparisons
- Zero configuration required

**Header**: `versionOSRuntime.h`

**Source**: `versionOSRuntime.c`

**Platforms**: Linux, macOS, and the BSD family - anything with `uname()`.
Version _detection_ is portable; the pre-defined `linuxKernelHas*` macros only
carry meaning on Linux (see [Platform Support](#platform-support)).

## Global State

```c
/* The running kernel's version (uname release), read at startup */
extern uint64_t versionOSRuntimeKernelVersion;
```

**Important**: This variable is read-only after initialization. Do not modify it.

## API Reference

### Version Comparison Macro

```c
/* Check if kernel version is greater than or equal to specified version
 * major: major version number
 * minor: minor version number
 * patch: patch version number
 * Returns: true if running kernel >= specified version
 */
#define osVersionGTE(major, minor, patch) \
    (versionOSRuntimeKernelVersion >= versionOSRuntimeCode(major, minor, patch))
```

This is a **runtime** comparison, not a preprocessor conditional: the right-hand
side folds to a constant, but the left-hand side is a global read. Use it in
ordinary `if` statements, never in `#if`.

### Linux Kernel Feature Detection

Pre-defined macros for common Linux kernel features. They compare the _running
kernel's_ version, so they are only meaningful when that kernel is Linux -
see the warning under [Platform Support](#platform-support).

```c
/* True if kernel supports SO_REUSEPORT (Linux 3.9.0+) */
#define linuxKernelHasREUSEPORT osVersionGTE(3, 9, 0)

/* True if kernel supports TCP Fast Open for clients (Linux 3.6.0+) */
#define linuxKernelHasTFOClient osVersionGTE(3, 6, 0)

/* True if kernel supports TCP Fast Open for servers on IPv4 (Linux 3.7.0+) */
#define linuxKernelHasTFOServerIPv4 osVersionGTE(3, 7, 0)

/* True if kernel supports TCP Fast Open for servers on IPv6 (Linux 3.16.0+) */
#define linuxKernelHasTFOServerIPv6 osVersionGTE(3, 16, 0)
```

## Version Encoding

Versions are encoded with `versionOSRuntimeCode`, 16 bits per part, so
the codes order as the versions do (a long-lived kernel's patch level passes
255, which an 8-bit field would spill into the minor version):

```c
#define versionOSRuntimeCode(major, minor, patch) \
    (((uint64_t)(major) << 32) | ((uint64_t)(minor) << 16) | (uint64_t)(patch))
```

**Examples:**

- Linux 3.9.0 → `0x0000000300090000`
- Linux 4.19.300 → `0x000000040013012C` (patch 300 = `0x012C`), below 4.20.0
- Linux 5.10.0 → `0x00000005000A0000`

## How It Works

### Automatic Initialization

A constructor reads `uname()` before `main()` and parses its release string
with `versionOSRuntimeParse`: the first three numeric parts, a missing part 0,
each clamped to 0..65535, stopping at the first character that is not a digit
or a dot (so `6.1.0-13-amd64` is 6.1.0 and `5.15` is 5.15.0). If `uname()`
fails the version stays 0. `versionOSRuntimeParse` is public, so a release
string from elsewhere encodes the same way.

**Key Points:**

- Runs before `main()` executes
- Parses kernel version from `uname().release`
- Thread-safe (completes before any user code runs)
- No initialization function needed
- `versionOSRuntimeKernelVersion` starts at 0, so every `osVersionGTE()` check
  would report false if the constructor somehow did not run

## Usage Examples

### Example 1: Basic Version Check

```c
#include "versionOSRuntime.h"
#include <stdio.h>

void printKernelInfo(void) {
    /* Extract individual version components */
    int major = (versionOSRuntimeKernelVersion >> 16) & 0xFF;
    int minor = (versionOSRuntimeKernelVersion >> 8) & 0xFF;
    int patch = versionOSRuntimeKernelVersion & 0xFF;

    printf("Running kernel: %d.%d.%d\n", major, minor, patch);

    /* Check specific version */
    if (osVersionGTE(5, 0, 0)) {
        printf("Running Linux 5.0 or newer\n");
    } else if (osVersionGTE(4, 0, 0)) {
        printf("Running Linux 4.x\n");
    } else if (osVersionGTE(3, 0, 0)) {
        printf("Running Linux 3.x\n");
    } else {
        printf("Running older kernel\n");
    }
}
```

**Output on Linux 5.10.0:**

```
Running kernel: 5.10.0
Running Linux 5.0 or newer
```

### Example 2: SO_REUSEPORT Support

```c
#include "versionOSRuntime.h"
#include <sys/socket.h>

int createListenSocket(const char *port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }

    /* Enable SO_REUSEADDR (always available) */
    int reuse = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Enable SO_REUSEPORT if kernel supports it */
#ifdef SO_REUSEPORT
    if (linuxKernelHasREUSEPORT) {
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT,
                      &reuse, sizeof(reuse)) == 0) {
            printf("SO_REUSEPORT enabled\n");
        }
    } else {
        printf("SO_REUSEPORT not supported (kernel < 3.9.0)\n");
    }
#endif

    /* ... bind and listen ... */
    return sockfd;
}
```

### Example 3: TCP Fast Open Detection

```c
#include "versionOSRuntime.h"

typedef struct TcpFeatures {
    bool tfoClient;
    bool tfoServerIPv4;
    bool tfoServerIPv6;
} TcpFeatures;

TcpFeatures detectTcpFeatures(void) {
    TcpFeatures features = {0};

    features.tfoClient = linuxKernelHasTFOClient;
    features.tfoServerIPv4 = linuxKernelHasTFOServerIPv4;
    features.tfoServerIPv6 = linuxKernelHasTFOServerIPv6;

    return features;
}

void printTcpFeatures(void) {
    TcpFeatures features = detectTcpFeatures();

    printf("TCP Fast Open Support:\n");
    printf("  Client:       %s\n", features.tfoClient ? "yes" : "no");
    printf("  Server IPv4:  %s\n", features.tfoServerIPv4 ? "yes" : "no");
    printf("  Server IPv6:  %s\n", features.tfoServerIPv6 ? "yes" : "no");
}
```

**Output on Linux 3.7.0:**

```
TCP Fast Open Support:
  Client:       yes
  Server IPv4:  yes
  Server IPv6:  no
```

**Output on Linux 3.16.0:**

```
TCP Fast Open Support:
  Client:       yes
  Server IPv4:  yes
  Server IPv6:  yes
```

### Example 4: Feature-Based Configuration

```c
#include "OSRegulate.h"  /* OSRegulateCPUCountGet */
#include "versionOSRuntime.h"

typedef struct ServerConfig {
    bool useReusePort;
    bool useTFO;
    int workerProcesses;
} ServerConfig;

ServerConfig getOptimalConfig(void) {
    ServerConfig config = {0};

    /* Use SO_REUSEPORT for load balancing across workers
     * only if kernel supports it */
    config.useReusePort = linuxKernelHasREUSEPORT;

    /* Enable TCP Fast Open if available */
    config.useTFO = linuxKernelHasTFOServerIPv4;

    /* If REUSEPORT available, spawn multiple workers
     * Otherwise, use single process with multiple threads */
    if (config.useReusePort) {
        config.workerProcesses = (int)OSRegulateCPUCountGet();
        printf("Using %d worker processes with SO_REUSEPORT\n",
               config.workerProcesses);
    } else {
        config.workerProcesses = 1;
        printf("SO_REUSEPORT not available, using single process\n");
    }

    return config;
}
```

### Example 5: Runtime Version Requirements

```c
#include "versionOSRuntime.h"
#include <stdio.h>
#include <stdlib.h>

/* Minimum kernel version required for application */
#define MIN_KERNEL_MAJOR 3
#define MIN_KERNEL_MINOR 10
#define MIN_KERNEL_PATCH 0

bool checkKernelRequirements(void) {
    if (!osVersionGTE(MIN_KERNEL_MAJOR, MIN_KERNEL_MINOR, MIN_KERNEL_PATCH)) {
        int major = (versionOSRuntimeKernelVersion >> 16) & 0xFF;
        int minor = (versionOSRuntimeKernelVersion >> 8) & 0xFF;
        int patch = versionOSRuntimeKernelVersion & 0xFF;

        fprintf(stderr, "ERROR: Kernel version too old\n");
        fprintf(stderr, "  Current:  %d.%d.%d\n", major, minor, patch);
        fprintf(stderr, "  Required: %d.%d.%d or newer\n",
                MIN_KERNEL_MAJOR, MIN_KERNEL_MINOR, MIN_KERNEL_PATCH);
        return false;
    }

    return true;
}

int main(void) {
    if (!checkKernelRequirements()) {
        return 1;
    }

    printf("Kernel version check passed\n");
    /* ... continue with program ... */
    return 0;
}
```

### Example 6: Custom Feature Detection

```c
#include "versionOSRuntime.h"

/* Custom feature checks for your application */
#define hasUserNamespaces    osVersionGTE(3, 8, 0)
#define hasBPF               osVersionGTE(3, 18, 0)
#define haseBPF              osVersionGTE(4, 1, 0)
#define hasUnifiedCgroups    osVersionGTE(4, 5, 0)
#define hasUringIO           osVersionGTE(5, 1, 0)

void printAvailableFeatures(void) {
    printf("Available kernel features:\n");

    if (hasUserNamespaces) {
        printf("  ✓ User namespaces\n");
    }

    if (hasBPF) {
        printf("  ✓ Classic BPF\n");
    }

    if (haseBPF) {
        printf("  ✓ Extended BPF (eBPF)\n");
    }

    if (hasUnifiedCgroups) {
        printf("  ✓ Unified cgroups (cgroup v2)\n");
    }

    if (hasUringIO) {
        printf("  ✓ io_uring\n");
    }
}

int main(void) {
    printAvailableFeatures();
    return 0;
}
```

## Implementation Details

### Constructor Execution Order

The `__attribute__((constructor))` ensures initialization happens:

- After static initialization
- Before `main()` executes
- Before any other code runs

This guarantees `versionOSRuntimeKernelVersion` is valid when accessed.

### Version Parsing

`versionOSRuntimeParse` reads the kernel version from `uname().release`:

**Example release strings:**

- `"5.10.0-8-amd64"` → `5.10.0` (Linux)
- `"4.15.0-112-generic"` → `4.15.0` (Linux)
- `"3.10.0-1160.el7.x86_64"` → `3.10.0` (Linux)
- `"23.6.0"` → `23.6.0` (macOS - this is the **Darwin** version, not macOS 14)
- `"5.15"` → `5.15.0`; `"6"` → `6.0.0`; `""` or `"abc"` → `0.0.0`

Only the first three numeric components are read; suffixes are ignored, a
missing component is 0, and a component above 65535 reads as 65535.

### Thread Safety

- Initialization completes before program starts (single-threaded)
- Read-only access after initialization (thread-safe)
- No locking required

### Performance

- **Initialization**: One-time cost at startup (one `uname()` and a short parse)
- **Version Checks**: A single integer comparison against a global
- **Memory Overhead**: One `uint64_t` variable

## Platform Support

| Platform   | uname() | Version Detection | Notes                                       |
| ---------- | ------- | ----------------- | ------------------------------------------- |
| Linux      | ✓       | ✓                 | Primary use case; feature macros meaningful |
| macOS      | ✓       | ✓                 | Reports the **Darwin** kernel version       |
| BSD family | ✓       | ✓                 | FreeBSD / NetBSD / OpenBSD / DragonFly      |

> **The `linuxKernelHas*` macros lie on non-Linux systems.** They compare
> `uname().release`, which on macOS is the Darwin version (23.x today) and on
> the BSDs is that kernel's own version. Every one of them therefore evaluates
> to _true_ on a modern macOS, even though SO_REUSEPORT-with-Linux-semantics and
> `/proc/sys/net/ipv4/tcp_fastopen` do not exist there. Pair them with a
> `#if DK_OS_LINUX` guard (or with the runtime probe in `OSRegulate`, which does
> exactly that) before acting on them.

On non-Linux platforms `osVersionGTE()` is still useful for building your own
checks against that platform's own kernel versioning.

## Best Practices

### 1. Use Pre-Defined Macros When Available

```c
/* GOOD - Use existing macro */
if (linuxKernelHasREUSEPORT) {
    /* ... */
}

/* BAD - Manually check version */
if (osVersionGTE(3, 9, 0)) {
    /* Less clear what feature this checks */
}
```

### 2. Define Semantic Feature Macros

```c
/* GOOD - Semantic feature names */
#define supportsModernNetworking osVersionGTE(4, 0, 0)
#define supportsSeccomp          osVersionGTE(3, 5, 0)

if (supportsModernNetworking) {
    enableAdvancedFeatures();
}

/* BAD - Opaque version numbers in code */
if (osVersionGTE(4, 0, 0)) {
    /* What feature needs 4.0? */
}
```

### 3. Provide Fallbacks

```c
/* GOOD - Graceful degradation */
if (linuxKernelHasREUSEPORT) {
    printf("Using SO_REUSEPORT for load balancing\n");
    enableReusePort();
} else {
    printf("SO_REUSEPORT unavailable, using thread pool\n");
    useThreadPool();
}

/* BAD - No fallback */
assert(linuxKernelHasREUSEPORT);  /* Crash on old kernels! */
```

### 4. Log Version Information

```c
/* GOOD - Log kernel version for debugging */
void logSystemInfo(void) {
    int major = (versionOSRuntimeKernelVersion >> 16) & 0xFF;
    int minor = (versionOSRuntimeKernelVersion >> 8) & 0xFF;
    int patch = versionOSRuntimeKernelVersion & 0xFF;

    printf("Kernel: %d.%d.%d\n", major, minor, patch);
    printf("SO_REUSEPORT: %s\n",
           linuxKernelHasREUSEPORT ? "available" : "not available");
}
```

### 5. Don't Modify the Global Variable

```c
/* BAD - Never do this! */
versionOSRuntimeKernelVersion = versionOSRuntimeCode(5, 0, 0);  /* Wrong! */

/* GOOD - Read-only access */
if (osVersionGTE(4, 15, 0)) {
    /* ... */
}
```

## Common Kernel Version Requirements

### Linux Kernel Features by Version

| Version | Notable Features             |
| ------- | ---------------------------- |
| 2.6.27  | epoll improvements           |
| 3.5.0   | seccomp mode 2 (filter)      |
| 3.6.0   | TCP Fast Open (client)       |
| 3.7.0   | TCP Fast Open (server, IPv4) |
| 3.8.0   | User namespaces              |
| 3.9.0   | SO_REUSEPORT                 |
| 3.16.0  | TCP Fast Open (server, IPv6) |
| 3.18.0  | Classic BPF                  |
| 4.1.0   | Extended BPF (eBPF)          |
| 4.5.0   | Unified cgroups (cgroup v2)  |
| 5.1.0   | io_uring                     |
| 5.6.0   | WireGuard                    |

## Use Cases

1. **Feature Detection**: Check if kernel supports specific features
2. **Graceful Degradation**: Provide fallbacks for older kernels
3. **Compatibility Testing**: Verify application works on target kernel versions
4. **Performance Optimization**: Enable newer features when available
5. **Security Features**: Detect seccomp, namespaces, or other security capabilities
6. **Minimum Version Enforcement**: Refuse to run on too-old kernels
7. **Conditional Compilation**: Enable/disable code based on runtime version

## Debugging

### Check Current Kernel Version

```bash
$ uname -r
5.10.0-8-amd64
```

### Verify Version Detection

```c
#include "versionOSRuntime.h"
#include <stdio.h>

int main(void) {
    printf("Encoded version: 0x%08zx\n", versionOSRuntimeKernelVersion);

    int major = (versionOSRuntimeKernelVersion >> 16) & 0xFF;
    int minor = (versionOSRuntimeKernelVersion >> 8) & 0xFF;
    int patch = versionOSRuntimeKernelVersion & 0xFF;

    printf("Detected version: %d.%d.%d\n", major, minor, patch);

    return 0;
}
```

## See Also

- [OSRegulate](OS_REGULATE.md) - the main consumer of these macros
  (`OSRegulateTFOMode()` gates on `linuxKernelHasTFOServerIPv4`)
- `uname(2)` - get system information
- Linux kernel documentation
- FreeBSD release notes
- macOS kernel versions (Darwin)

## Testing

The versionOSRuntime module is automatically tested at program startup. To verify:

1. Check that `versionOSRuntimeKernelVersion` is non-zero
2. Compare detected version with `uname -r` output
3. Test feature macros against known kernel versions
4. Run on different kernel versions to verify compatibility

**Test Program:**

```c
#include "versionOSRuntime.h"
#include <stdio.h>
#include <sys/utsname.h>

int main(void) {
    struct utsname info;
    uname(&info);

    int major = (versionOSRuntimeKernelVersion >> 16) & 0xFF;
    int minor = (versionOSRuntimeKernelVersion >> 8) & 0xFF;
    int patch = versionOSRuntimeKernelVersion & 0xFF;

    printf("uname.release: %s\n", info.release);
    printf("Detected:      %d.%d.%d\n", major, minor, patch);
    printf("Encoded:       0x%08zx\n", versionOSRuntimeKernelVersion);

    /* Test feature macros */
    printf("\nFeature Detection:\n");
    printf("  SO_REUSEPORT:     %s\n", linuxKernelHasREUSEPORT ? "yes" : "no");
    printf("  TFO Client:       %s\n", linuxKernelHasTFOClient ? "yes" : "no");
    printf("  TFO Server IPv4:  %s\n", linuxKernelHasTFOServerIPv4 ? "yes" : "no");
    printf("  TFO Server IPv6:  %s\n", linuxKernelHasTFOServerIPv6 ? "yes" : "no");

    return 0;
}
```
