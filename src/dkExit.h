#pragma once

/* Leaving the process at once: _exit (no exit handlers, no stdio flush).
 * A coverage build writes the process's profile first, as a normal exit
 * would, so the calls the process made are counted.  The profile's name
 * comes from LLVM_PROFILE_FILE with this process's id: the runtime named
 * it once, in the process that started the program, so a forked process
 * names its own before writing (two processes writing one file corrupt
 * it). */

#include <unistd.h>

#ifdef DATAKIT_COVERAGE
#include <stdlib.h> /* getenv */
void __llvm_profile_set_filename(const char *name);
int __llvm_profile_write_file(void);
#endif

[[noreturn]] [[maybe_unused]] static void dkExitNow(int status) {
#ifdef DATAKIT_COVERAGE
    const char *pattern = getenv("LLVM_PROFILE_FILE");
    if (pattern) {
        __llvm_profile_set_filename(pattern);
        (void)__llvm_profile_write_file();
    }
#endif
    _exit(status);
}
