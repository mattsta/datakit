#pragma once

#include "datakit.h"

/* A version a.b.c as one number that orders as the versions do: 16 bits
 * per part (a long-lived kernel's patch level passes 255) */
#define versionOSRuntimeCode(major, minor, patch)                              \
    (((uint64_t)(major) << 32) | ((uint64_t)(minor) << 16) | (uint64_t)(patch))

/* The running kernel's version (uname release), read at startup */
extern uint64_t versionOSRuntimeKernelVersion;

#define osVersionGTE(major, minor, patch)                                      \
    (versionOSRuntimeKernelVersion >= versionOSRuntimeCode(major, minor, patch))

#define linuxKernelHasREUSEPORT osVersionGTE(3, 9, 0)
#define linuxKernelHasTFOClient osVersionGTE(3, 6, 0)
#define linuxKernelHasTFOServerIPv4 osVersionGTE(3, 7, 0)
#define linuxKernelHasTFOServerIPv6 osVersionGTE(3, 16, 0)

/* The version code of a uname release string such as "6.1.0-13-amd64" or
 * "23.6.0": its first three numeric parts, a missing part 0, each part
 * clamped to 0..65535 */
uint64_t versionOSRuntimeParse(const char *release);

#ifdef DATAKIT_TEST
int versionOSRuntimeTest(int argc, char *argv[]);
#endif
