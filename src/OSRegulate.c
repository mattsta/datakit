#include "OSRegulate.h"
#include "versionOSRuntime.h"

#include <errno.h>        /* strerror */
#include <fcntl.h>        /* open */
#include <stdio.h>        /* snprintf */
#include <sys/resource.h> /* getrlimit */
#include <unistd.h>       /* _exit */

/* rusage */
#include <sys/resource.h>
#include <sys/time.h>

#if DK_OS_LINUX
#include <sys/prctl.h> /* for OSRegulateRequestSignalChildWhenParentExits() */
#endif

/* If errorMsg provided, set then return error.
 * If not provided, just return error */
#define returnError(m, e)                                                      \
    do {                                                                       \
        if (m) {                                                               \
            *(m) = (e);                                                        \
        }                                                                      \
        return OS_REGULATE_DAEMONIZE_ERROR;                                    \
    } while (0)

/* shorthand for returnError */
#define re(msg) returnError(errMsg, msg)

/* ====================================================================
 * Process Helpers
 * ==================================================================== */
OSRegulateDaemonizeStatus OSRegulateDaemonize(const char **errMsg) {
    switch (fork()) {
    case -1:
        /* error */
        re("Failure to fork() daemon");
    case 0:
        /* child */
        break;
    default:
        /* got pid of child, shutdown parent */
        return OS_REGULATE_DAEMONIZE_PARENT;
    }

    /* create new session id for child so it gets detached from parent */
    if (setsid() == -1) {
        re("Failed to run setsid()");
    }

    /* close default FDs (like daemonize()) */
    const int fd = open("/dev/null", O_RDWR, 0);
    if (fd == -1) {
        re("Failed to open /dev/null");
    }

    if (dup2(fd, STDIN_FILENO) == -1) {
        re("Failed to dup2 stdin");
    }

    if (dup2(fd, STDOUT_FILENO) == -1) {
        re("Failed to dup2 stdout");
    }

    if (fd > STDERR_FILENO) {
        if (close(fd) == -1) {
            re("Failed to close /dev/null");
        }
    }

    return OS_REGULATE_DAEMONIZE_CHILD;
}

void OSRegulateDaemonizeThenExit(OSRegulateDaemonizeParentExitCallback *cb,
                                 void *userData) {
    if (!OSRegulateDaemonize(NULL)) {
        /* Now we're the parent process, but we need to wait until the child
         * process is ready before we exit. */
        while (!cb(userData)) {
            nanosleep(&(struct timespec){1, 0}, NULL);
        }

        OSRegulateExitClean();
    }
}

void OSRegulateDaemonizeThenExitNoWait(void) {
    if (!OSRegulateDaemonize(NULL)) {
        /* parent process exits cleanly */
        OSRegulateExitClean();
    }

    /* child process continues */
}

bool OSRegulateWritePidToFile(char *path) {
    FILE *fp = fopen(path, "w");
    if (fp) {
        /* Lock the pid so we aren't running twice */
        if (flock(fileno(fp), LOCK_EX | LOCK_NB) != 0) {
            return false;
        }

        fprintf(fp, "%d\n", (int)getpid());
        fflush(fp);

#if 0
        /* Don't close pid FD because we want to keep it locked */
        fclose(fp);
#endif

        return true;
    }

    return false;
}

void OSRegulateExitClean(void) {
    _exit(0);
    __builtin_unreachable();
}

bool OSRegulateRequestSignalChildWhenParentExits(int sig) {
#if DK_OS_LINUX
    /* We don't version check this because it has been available since
     * Linux 2.1.57 (1997-09-25) */
    return prctl(PR_SET_PDEATHSIG, sig) == 0;
#else
    (void)sig;
    return false;
#endif
}

bool OSRegulateParentStillExisits(void) {
    /* If parent is '1' we got reparented!
     * Obviously doesn't apply to daemons directly attached to init. */
    return !(getppid() == 1);
}

/* ====================================================================
 * CPUs
 * ==================================================================== */
#include <sched.h>

size_t OSRegulateCPUCountGet(void) {
    ssize_t cpuCount = 1;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    cpuCount = si.dwNumberOfProcessors;
#if 0
#elif defined(_GNU_SOURCE) && defined(CPU_COUNT)
    cpu_set_t set;
    /* this crashes the process on mah linux server? */
    pthread_getaffinity_np(pthread_self(), sizeof(set), &set);
    cpuCount = CPU_COUNT(&set);
#endif
#elif defined(_SC_NPROCESSORS_ONLN)
    cpuCount = sysconf(_SC_NPROCESSORS_ONLN);
#endif

    if (cpuCount == -1) {
        return 1;
    }

    return cpuCount;
}

/* ====================================================================
 * Networking
 * ==================================================================== */
/* Modes:
 *  - 1: Client
 *  - 2: Server
 *  - 3: Client and Server */
int OSRegulateTFOMode() {
    int tfo = 0;

    if (linuxKernelHasTFOServerIPv4) {
        FILE *fp = fopen("/proc/sys/net/ipv4/tcp_fastopen", "r");

        if (fp) {
            char buf[64] = {0};
            if (fgets(buf, sizeof(buf), fp) != NULL) {
                tfo = atoi(buf);
            }

            fclose(fp);
        }
    }

    return tfo;
}

bool OSRegulateTcpBacklogMeetsLimit(int tcpBacklogListenLength) {
#if HAVE_PROC_SOMAXCONN
    FILE *fp = fopen("/proc/sys/net/core/somaxconn", "r");
    char buf[64] = {0};
    bool limitFits = true;

    if (!fp) {
        return false;
    }

    if (fgets(buf, sizeof(buf), fp) != NULL) {
        const int somaxconn = atoi(buf);
        if (somaxconn > 0 && somaxconn < tcpBacklogListenLength) {
            limitFits = false;
        }
    }

    fclose(fp);
    return limitFits;
#else
    (void)tcpBacklogListenLength;
    return true;
#endif
}

/* ====================================================================
 * Files
 * ==================================================================== */
bool OSRegulateAdjustOpenFilesLimit(const size_t requestedFdCount,
                                    size_t *const limitActuallySet,
                                    char **statusMsg) {
    static char statusmsg[1024] = {0};

    /* Estimate of minimum FDs used internally before any user actions */
    static const rlim_t fdOverhead = 64;

    /* Number of fds to request from system */
    const rlim_t targetLimit = requestedFdCount + fdOverhead;

    if (statusMsg) {
        *statusMsg = statusmsg;
    }

    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) == -1) {
        snprintf(statusmsg, sizeof(statusmsg),
                 "Couldn't get current open file limit (%s)", strerror(errno));
        return false;
    }

    const rlim_t originalLimit = limit.rlim_cur;

    /* System limit is already greater than the number we're
     * requesting, so tell the caller what the current
     * number is and return success */
    if (originalLimit >= targetLimit) {
        if (limitActuallySet) {
            *limitActuallySet = originalLimit;
        }

        return true;
    }

    /* else, try to increase NOFILES ourself */
    rlim_t bestlimit = targetLimit;
    int setrlimit_error = 0;

    /* Try to set the file limit to match 'targetLimit' or at least
     * to the higher value supported less than targetLimit. */
    while (bestlimit > originalLimit) {
        const rlim_t decr_step = 16;

        limit.rlim_cur = bestlimit;
        limit.rlim_max = bestlimit;
        if (setrlimit(RLIMIT_NOFILE, &limit) != -1) {
            break;
        }

        setrlimit_error = errno;

        /* We failed to set file limit to 'bestlimit'. Try with a
         * smaller limit decrementing by a few FDs per iteration. */
        if (bestlimit < decr_step) {
            break;
        }

        bestlimit -= decr_step;
    }

    /* Assume that the limit we get initially is still valid if
     * our last try was even lower. */
    if (bestlimit < originalLimit) {
        bestlimit = originalLimit;
    }

    if (limitActuallySet) {
        *limitActuallySet = bestlimit;
    }

    if (bestlimit < targetLimit) {
        /* maxclients is unsigned so may overflow: in order
         * to check if maxclients is now logically less than 1
         * we test indirectly via bestlimit. */
        if (bestlimit <= fdOverhead) {
            snprintf(statusmsg, sizeof(statusmsg),
                     "Your current 'ulimit -n' "
                     "of %" PRIu64 " is not enough for the server to start. "
                     "Increase your open file limit to at least "
                     "%" PRIu64 ".",
                     (uint64_t)originalLimit, (uint64_t)targetLimit);
            return false;
        }

        snprintf(statusmsg, sizeof(statusmsg),
                 "Server can't set maximum open files "
                 "to %" PRIu64 " because of OS error: %s.",
                 (uint64_t)targetLimit, strerror(setrlimit_error));

        /* 'false' because we changed limit,
         * BUT not as much as requested. */
        return false;
    }

    snprintf(statusmsg, sizeof(statusmsg),
             "Increased maximum number of open files "
             "to %" PRIu64 " (was originally %" PRIu64 ").",
             (uint64_t)targetLimit, (uint64_t)originalLimit);

    /* 'true' because we successfully changed limit. */
    return true;
}

/* ====================================================================
 * Usage
 * ==================================================================== */
bool OSRegulateResourceUsageGet(long *selfMaxRSS, long *childMaxRSS,
                                float *selfSystemCPU, float *selfUserCPU,
                                float *childSystemCPU, float *childUserCPU) {
    struct rusage self;
    struct rusage children;
    if (getrusage(RUSAGE_SELF, &self) != 0) {
        return false;
    }

    if (getrusage(RUSAGE_CHILDREN, &children) != 0) {
        return false;
    }

#if DK_OS_APPLE
    /* OS X reports size in bytes */
    *selfMaxRSS = self.ru_maxrss;
    *childMaxRSS = children.ru_maxrss;
#else
    /* Linux and FreeBSD report size in KB */
    *selfMaxRSS = self.ru_maxrss * 1024ULL;
    *childMaxRSS = children.ru_maxrss * 1024ULL;
#endif

    *selfSystemCPU =
        (float)self.ru_stime.tv_sec + (float)self.ru_stime.tv_usec / 1000000;

    *selfUserCPU =
        (float)self.ru_utime.tv_sec + (float)self.ru_utime.tv_usec / 1000000;

    *childSystemCPU = (float)children.ru_stime.tv_sec +
                      (float)children.ru_stime.tv_usec / 1000000;

    *childUserCPU = (float)children.ru_utime.tv_sec +
                    (float)children.ru_utime.tv_usec / 1000000;

    return true;
}

/* ====================================================================
 * Memory
 * ==================================================================== */
#if DK_OS_LINUX
bool OSRegulateLinuxOvercommitEnabled(void) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory", "r");
    char buf[64] = {0};

    if (!fp) {
        return false;
    }

    if (fgets(buf, 64, fp) == NULL) {
        fclose(fp);
        return false;
    }

    fclose(fp);

    const int overcommitSetting = atoi(buf);

    /* Ovecommit of 0 or 1 is enabled, 2 is disabled. */
    return overcommitSetting <= 1;
}

bool OSRegulateLinuxTransparentHugePagesEnabled(void) {
    FILE *fp = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    char buf[1024] = {0};

    if (!fp) {
        return false;
    }

    if (fgets(buf, sizeof(buf), fp) == NULL) {
        fclose(fp);
        return false;
    }

    fclose(fp);

    /* If '[never]' is selected, 'strstr' finds it
     * and returns a value (so if found, setting is disabled),
     * else, '[never]' isn't found meaning THP is enabled,
     * so return true if NOT found (meaning enabled) and
     *    return false if FOUND (meaning disabled) */
    return strstr(buf, "[never]") == NULL;
}

bool OSRegulateLinuxMemorySettingsAreOkay(void) {
    if (OSRegulateLinuxOvercommitEnabled() &&
        !OSRegulateLinuxTransparentHugePagesEnabled()) {
        return true;
    }

    return false;
}

/* Get the sum of the specified field (converted form kb to bytes) in
 * /proc/self/smaps. The field must be specified with trailing ":" as it
 * apperas in the smaps output.
 *
 * If a pid is specified, the information is extracted for such a pid,
 * otherwise if pid is -1 the information is reported is about the
 * current process.
 *
 * Example: zmalloc_get_smap_bytes_by_field("Rss:",-1);
 */
size_t OSRegulateLinuxSmapBytesByFieldForPid(const char *field, int64_t pid) {
#if HAVE_PROC_SMAPS
    char line[1024] = {0};
    size_t bytes = 0;
    int flen = strlen(field);
    FILE *fp;

    if (pid == -1) {
        fp = fopen("/proc/self/smaps", "r");
    } else {
        char filename[128];
        snprintf(filename, sizeof(filename), "/proc/%ld/smaps", pid);
        fp = fopen(filename, "r");
    }

    if (!fp) {
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, field, flen) == 0) {
            char *const p = strchr(line, 'k');
            if (p) {
                *p = '\0';
                bytes += strtol(line + flen, NULL, 10) * 1024;
            }
        }
    }

    fclose(fp);
    return bytes;
#else
    (void)field;
    (void)pid;
    return 0;
#endif
}

int OSRegulateLinuxTransparentHugePagesGetAnonHugePagesSize(int64_t pid) {
    return OSRegulateLinuxSmapBytesByFieldForPid("AnonHugePages:", pid);
}

size_t OSRegualteLinuxSmapPrivateDirtyGet(int64_t pid) {
    return OSRegulateLinuxSmapBytesByFieldForPid("Private_Dirty:", pid);
}
#endif

#if defined(HAVE_PROC_STAT)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

size_t OSRegulateRSSGet(void) {
    int page = sysconf(_SC_PAGESIZE);
    size_t rss;
    char buf[4096];
    char filename[256];
    int fd;
    int count;
    char *p;
    char *x;

    snprintf(filename, 256, "/proc/%d/stat", getpid());
    if ((fd = open(filename, O_RDONLY)) == -1) {
        return 0;
    }

    if (read(fd, buf, 4096) <= 0) {
        close(fd);
        return 0;
    }

    close(fd);

    p = buf;
    count = 23; /* RSS is the 24th field in /proc/<pid>/stat */
    while (p && count--) {
        p = strchr(p, ' ');
        if (p) {
            p++;
        }
    }

    if (!p) {
        return 0;
    }

    x = strchr(p, ' ');
    if (!x) {
        return 0;
    }

    *x = '\0';

    rss = strtoll(p, NULL, 10);
    rss *= page;
    return rss;
}
#elif defined(HAVE_TASKINFO)
#include <mach/mach_init.h>
#include <mach/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

size_t OSRegulateRSSGet(void) {
    task_t task = MACH_PORT_NULL;
    struct task_basic_info t_info = {0};
    mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

    if (task_for_pid(current_task(), getpid(), &task) != KERN_SUCCESS) {
        return 0;
    }

    task_info(task, TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count);

    return t_info.resident_size;
}
#else
size_t OSRegulateRSSGet(void) {
    return -1;
}
#endif

/* Returns the size of physical memory (RAM) in bytes.
 * It looks ugly, but this is the cleanest way to achive cross platform results.
 * Cleaned up from:
 *
 * http://nadeausoftware.com/articles/2012/09/c_c_tip_how_get_physical_memory_size_system
 *
 * Note that this function:
 * 1) Was released under the following CC attribution license:
 *    http://creativecommons.org/licenses/by/3.0/deed.en_US.
 * 2) Was originally implemented by David Robert Nadeau.
 * 3) Was modified for Redis by Matt Stancliff.
 * 4) This note exists in order to comply with the original license.
 */
size_t OSRegulateTotalMemoryGet(void) {
#if defined(__unix__) || defined(__unix) || defined(unix) ||                   \
    (defined(__APPLE__) && defined(__MACH__))
#if defined(CTL_HW) && (defined(HW_MEMSIZE) || defined(HW_PHYSMEM64))
    int mib[2] = {0};
    mib[0] = CTL_HW;
#if defined(HW_MEMSIZE)
    mib[1] = HW_MEMSIZE; /* OSX. --------------------- */
#elif defined(HW_PHYSMEM64)
    mib[1] = HW_PHYSMEM64; /* NetBSD, OpenBSD. --------- */
#endif
    int64_t size = 0; /* 64-bit */
    size_t len = sizeof(size);
    if (sysctl(mib, 2, &size, &len, NULL, 0) == 0) {
        return (size_t)size;
    }
    return 0; /* Failed? */

#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    /* FreeBSD, Linux, OpenBSD, and Solaris. -------------------- */
    return (size_t)sysconf(_SC_PHYS_PAGES) * (size_t)sysconf(_SC_PAGESIZE);

#elif defined(CTL_HW) && (defined(HW_PHYSMEM) || defined(HW_REALMEM))
    /* DragonFly BSD, FreeBSD, NetBSD, OpenBSD, and OSX. -------- */
    int mib[2] = {0};
    mib[0] = CTL_HW;
#if defined(HW_REALMEM)
    mib[1] = HW_REALMEM; /* FreeBSD. ----------------- */
#elif defined(HW_PYSMEM)
    mib[1] = HW_PHYSMEM; /* Others. ------------------ */
#endif
    uint32_t size = 0; /* 32-bit */
    size_t len = sizeof(size);
    if (sysctl(mib, 2, &size, &len, NULL, 0) == 0) {
        return (size_t)size;
    }

    return 0; /* Failed? */
#else
    return 0; /* Unknown method to get the data. */
#endif
#else
    return 0; /* Unknown OS. */
#endif
}

/* ====================================================================
 * Network Validators
 * ==================================================================== */
/* RFC 6890 notes special IP spaces in general */
/* RFC 1918 specifies IPv4 special addresses */
/* RFC 4193 specifies IPv6 special addresses */
/* We're ignoring some of the more esoteric "benchmarking" networks for now. */
bool OSRegulateNetworkIsPrivate(const struct addrinfo *ai) {
    if (ai->ai_family == AF_INET) {
        return OSRegulateNetworkIsPrivateIPv4(
            (const struct sockaddr_in *)ai->ai_addr);
    }

    return OSRegulateNetworkIsPrivateIPv6(
        (const struct sockaddr_in6 *)ai->ai_addr);
}

bool OSRegulateNetworkIsPrivateIPv4(const struct sockaddr_in *const sa) {
    const uint8_t *netAddr = (uint8_t *)&sa->sin_addr.s_addr;

    /* The address integer in 'sa' is big endian. */

    /* RFC 1918 */
    /* 127.0.0.0/8 */
    if (netAddr[0] == 127) {
        return true;
    }

    /* 192.168.0.0/16 */
    if (netAddr[0] == 192 && netAddr[1] == 168) {
        return true;
    }

    /* 172.16.0.0/12 */
    if (netAddr[0] == 172 && (netAddr[1] >= 16 && netAddr[1] <= 31)) {
        return true;
    }

    return false;
}

bool OSRegulateNetworkIsPrivateIPv6(const struct sockaddr_in6 *const sa6) {
    const uint8_t *netAddr = sa6->sin6_addr.s6_addr;

    /* RFC 4193 section 3: Local IPv6 Unicast Addresses */
    /* fc00::/7 */
    if (netAddr[0] == 0xfc || netAddr[0] == 0xfd) {
        return true;
    }

    /* RFC 4291 section 2.5.3: The Loopback Address */
    /* ::1/128 */
    /* We have to verify the first 15 bytes are zero and the final
     * byte is just 1 */
    bool is1 = true;
    for (size_t i = 0; i < 15; i++) {
        if (netAddr[i] != 0) {
            is1 = false;
            break;
        }
    }

    if (is1 && netAddr[15] == 1) {
        return true;
    }

    /* RFC 4291 section 2.5.6: Link-Local IPv6 Unicast Addresses */
    /* fe80::/10 */
    if (netAddr[0] == 0xfe && (netAddr[1] >= 0x80 && netAddr[1] <= 0xbf)) {
        return true;
    }

    return false;
}

bool OSRegulateNetworkIsAll(const struct addrinfo *ai) {
    if (ai->ai_family == AF_INET) {
        return OSRegulateNetworkIsAllIPv4(
            (const struct sockaddr_in *)ai->ai_addr);
    }

    return OSRegulateNetworkIsAllIPv6((const struct sockaddr_in6 *)ai->ai_addr);
}

bool OSRegulateNetworkIsAllIPv4(const struct sockaddr_in *const sa) {
    const uint32_t netAddr = sa->sin_addr.s_addr;

    if (netAddr != 0) {
        return false;
    }

    return true;
}

bool OSRegulateNetworkIsAllIPv6(const struct sockaddr_in6 *const sa6) {
    const uint8_t *netAddr = sa6->sin6_addr.s6_addr;
    for (size_t i = 0; i < 16; i++) {
        if (netAddr[i] != 0) {
            return false;
        }
    }

    return true;
}
