#pragma once

/* Leaving the process at once: _exit (no exit handlers, no stdio flush).
 * A coverage build writes the process's profile first, as a normal exit
 * would, so the calls the process made are counted (the coverage build
 * names one profile file per process). */

#include <unistd.h>

#ifdef DATAKIT_COVERAGE
int __llvm_profile_write_file(void);
#endif

[[noreturn]] [[maybe_unused]] static void dkExitNow(int status) {
#ifdef DATAKIT_COVERAGE
    (void)__llvm_profile_write_file();
#endif
    _exit(status);
}
