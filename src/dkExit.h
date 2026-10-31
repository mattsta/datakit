#pragma once

/* Leaving the process at once: _exit (no exit handlers, no stdio flush).
 * A coverage build writes the process's profile first, as a normal exit
 * would, so the calls the process made are counted.  The runtime filled
 * LLVM_PROFILE_FILE's %p once, in the process that started the program,
 * and a forked process would write that process's file (two writers
 * corrupt it): the name is filled again with this process's id. */

#include <unistd.h>

#ifdef DATAKIT_COVERAGE
#include <inttypes.h> /* PRIdMAX */
#include <limits.h>   /* PATH_MAX */
#include <stdio.h>    /* snprintf */
#include <stdlib.h>   /* getenv */
void __llvm_profile_set_filename(const char *name);
int __llvm_profile_write_file(void);
#endif

[[noreturn]] [[maybe_unused]] static void dkExitNow(int status) {
#ifdef DATAKIT_COVERAGE
    const char *pattern = getenv("LLVM_PROFILE_FILE");
    char name[PATH_MAX];
    size_t at = 0;
    for (const char *p = pattern; p && *p && at + 24 < sizeof(name); p++) {
        if (p[0] == '%' && p[1] == 'p') {
            at += (size_t)snprintf(name + at, sizeof(name) - at, "%" PRIdMAX,
                                   (intmax_t)getpid());
            p++;
        } else {
            name[at++] = *p;
        }
    }
    name[at] = '\0';
    if (at) {
        __llvm_profile_set_filename(name);
        (void)__llvm_profile_write_file();
    }
#endif
    _exit(status);
}
