#include "versionOSRuntime.h"
#include <sys/utsname.h>

uint64_t versionOSRuntimeKernelVersion = 0;

uint64_t versionOSRuntimeParse(const char *release) {
    uint64_t parts[3] = {0, 0, 0};
    size_t part = 0;
    const char *p = release;
    while (p && part < 3 && *p >= '0' && *p <= '9') {
        uint64_t v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (uint64_t)(*p - '0');
            if (v > 0xFFFF) {
                v = 0xFFFF; /* keep counting digits, stay clamped */
            }
            p++;
        }
        parts[part++] = v;
        if (*p != '.') {
            break;
        }
        p++;
    }

    return versionOSRuntimeCode(parts[0], parts[1], parts[2]);
}

/* Read once at startup: avoids a race on first use and keeps the read
 * off every later path */
[[gnu::constructor]] static void versionOSRuntimeInit(void) {
    struct utsname unameFields;
    if (uname(&unameFields) == 0) {
        versionOSRuntimeKernelVersion =
            versionOSRuntimeParse(unameFields.release);
    }
}

#ifdef DATAKIT_TEST
#include "ctest.h"

int versionOSRuntimeTest(int, char *[]) {
    int err = 0;

    TEST("release strings parse to ordered version codes") {
        static const struct {
            const char *release;
            uint64_t major, minor, patch;
        } cases[] = {
            {"6.1.0-13-amd64", 6, 1, 0},
            {"23.6.0", 23, 6, 0},
            {"4.19.300", 4, 19, 300},
            {"5.15", 5, 15, 0},
            {"6", 6, 0, 0},
            {"3.10.0-1160.el7.x86_64", 3, 10, 0},
            {"999999.2.3", 0xFFFF, 2, 3},
            {"", 0, 0, 0},
            {"abc", 0, 0, 0},
            {"1.2.3.4", 1, 2, 3},
            {"5.4abc", 5, 4, 0},
            {"7~rc1", 7, 0, 0},
        };
        for (size_t i = 0; i < countof(cases); i++) {
            const uint64_t got = versionOSRuntimeParse(cases[i].release);
            const uint64_t want = versionOSRuntimeCode(
                cases[i].major, cases[i].minor, cases[i].patch);
            if (got != want) {
                ERR("\"%s\" parsed to %#" PRIx64 ", want %#" PRIx64,
                    cases[i].release, got, want);
            }
        }
        if (versionOSRuntimeParse(nullptr) != 0) {
            ERR("NULL release parsed");
        }

        /* A patch past 255 stays below the next minor version */
        if (!(versionOSRuntimeParse("4.19.300") <
              versionOSRuntimeParse("4.20.0")) ||
            !(versionOSRuntimeParse("4.19.300") >
              versionOSRuntimeParse("4.19.299")) ||
            !(versionOSRuntimeParse("3.9.0") <
              versionOSRuntimeParse("3.10.0"))) {
            ERR("version codes out of order");
        }

        /* The running kernel was read at startup */
        if (versionOSRuntimeKernelVersion == 0 || !osVersionGTE(1, 0, 0)) {
            ERR("the running kernel version was not read");
        }
        if (osVersionGTE(65535, 65535, 65534)) {
            ERR("a version above any real kernel compared as reached");
        }
    }

    TEST_FINAL_RESULT;
}
#endif
