#include "versionOSRuntime.h"
#include <stdio.h>
#include <sys/utsname.h>

size_t versionOSRuntimeKernelVersion = 0;

/* Use constructor to always init this on startup.
 * Avoids race conditions and runtime latency. */
static void __attribute__((constructor)) init() {
    struct {
        int major;
        int minor;
        int patch;
    } kernelVer;

    struct utsname unameFields;
    uname(&unameFields);

    /* Extract version number for comparison... */
    sscanf(unameFields.release, "%d.%d.%d", &kernelVer.major, &kernelVer.minor,
           &kernelVer.patch);

    versionOSRuntimeKernelVersion =
        _DK_MK_VERSION(kernelVer.major, kernelVer.minor, kernelVer.patch);
}
