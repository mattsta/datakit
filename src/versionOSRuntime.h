#pragma once

#include "datakit.h"

extern size_t versionOSRuntimeKernelVersion;

#define osVersionGTE(major, minor, patch)                                      \
    (versionOSRuntimeKernelVersion >= _DK_MK_VERSION(major, minor, patch))

#define linuxKernelHasREUSEPORT osVersionGTE(3, 9, 0)
#define linuxKernelHasTFOClient osVersionGTE(3, 6, 0)
#define linuxKernelHasTFOServerIPv4 osVersionGTE(3, 7, 0)
#define linuxKernelHasTFOServerIPv6 osVersionGTE(3, 16, 0)
