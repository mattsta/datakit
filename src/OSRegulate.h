#pragma once

#include "datakit.h"
#include <arpa/inet.h> /* sockaddr[6]_in, inet_pton, ... */

/* struct addrinfo */
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef bool(OSRegulateDaemonizeParentExitCallback)(void *userData);

typedef enum OSRegulateDaemonizeStatus {
    OS_REGULATE_DAEMONIZE_PARENT,
    OS_REGULATE_DAEMONIZE_CHILD,
    OS_REGULATE_DAEMONIZE_ERROR
} OSRegulateDaemonizeStatus;

/* Process */
OSRegulateDaemonizeStatus OSRegulateDaemonize(const char **errMsg);
void OSRegulateDaemonizeThenExit(OSRegulateDaemonizeParentExitCallback *cb,
                                 void *userData);
bool OSRegulateWritePidToFile(char *path);
void OSRegulateDaemonizeThenExitNoWait(void);
void OSRegulateExitClean(void);

bool OSRegulateRequestSignalChildWhenParentExits(int sig);
bool OSRegulateParentStillExisits(void);

/* CPUs */
size_t OSRegulateCPUCountGet(void);

/* Networking */
bool OSRegulateTcpBacklogMeetsLimit(int tcpBacklogListenLength);
int OSRegulateTFOMode();

/* Files */
bool OSRegulateAdjustOpenFilesLimit(const size_t requestedFdCount,
                                    size_t *limitActuallySet, char **statusMsg);

/* Usage */
bool OSRegulateResourceUsageGet(long *selfMaxRSS, long *childMaxRSS,
                                float *selfSystemCPU, float *selfUserCPU,
                                float *childSystemCPU, float *childUserCPU);

/* Memory */
bool OSRegulateLinuxOvercommitEnabled(void);
bool OSRegulateLinuxTransparentHugePagesEnabled(void);
size_t OSRegulateLinuxSmapBytesByFieldForPid(const char *field, int64_t pid);
int OSRegulateLinuxTransparentHugePagesGetAnonHugePagesSize(int64_t pid);
size_t OSRegualteLinuxSmapPrivateDirtyGet(int64_t pid);
bool OSRegulateLinuxMemorySettingsAreOkay(void);
size_t OSRegulateTotalMemoryGet(void);
size_t OSRegulateRSSGet(void);

/* Network */
bool OSRegulateNetworkIsPrivate(const struct addrinfo *ai);
bool OSRegulateNetworkIsPrivateIPv4(const struct sockaddr_in *sa);
bool OSRegulateNetworkIsPrivateIPv6(const struct sockaddr_in6 *sa6);

bool OSRegulateNetworkIsAll(const struct addrinfo *ai);
bool OSRegulateNetworkIsAllIPv4(const struct sockaddr_in *sa);
bool OSRegulateNetworkIsAllIPv6(const struct sockaddr_in6 *sa6);
