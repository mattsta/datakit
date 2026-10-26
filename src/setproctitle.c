/* This is modified from nginx/src/os/unix/setproctitle.c (BSD-2) */

#include "datakit.h"
#include <assert.h>

#if DK_OS_LINUX || DK_OS_APPLE
#include "setproctitle.h"
#define SETPROCTITLE_PAD '\0'

/* Original comment from nginx */
/*
 * To change the process title in Linux we have to set argv[1]
 * to NULL and to copy the title to the same place where the argv[0] points to.
 * However, argv[0] may be too small to hold a new title.  Fortunately, Linux
 * stores argv[] and environ[] one after another.  So we should
 * ensure that is the continuous memory and then we allocate the new memory
 * for environ[] and copy it.  After this we could use the memory starting
 * from argv[0] for our process title.
 */

extern char **environ;

static const char *os_argv_last = nullptr;
static char **global_os_argv = nullptr;

bool setProctitleInit(char *os_argv[]) {
    size_t size = 0;

    /* We need to save argv[] itself for setProctitle() later. */
    global_os_argv = os_argv;

    for (size_t i = 0; environ[i]; i++) {
        size += strlen(environ[i]) + 1;
    }

    /* @own: static */
    char *p = zmalloc(size);
    os_argv_last = os_argv[0];

    for (size_t i = 0; os_argv[i]; i++) {
        if (os_argv_last == os_argv[i]) {
            os_argv_last = os_argv[i] + strlen(os_argv[i]) + 1;
        }
    }

    for (size_t i = 0; environ[i]; i++) {
        if (os_argv_last == environ[i]) {
            size = strlen(environ[i]) + 1;
            os_argv_last = environ[i] + size;

            memcpy(p, environ[i], size); /* size includes the NUL */
            environ[i] = (char *)p;
            p += size;
        }
    }

    os_argv_last--;
    os_argv[1] = nullptr;

    return true;
}

/* This function prototype matches the name and argument type provided by other
 * BSD platforms, so if we are on a bSD platform we will use the native version
 * instead of this. */
void setproctitle(char *title) {
    assert(os_argv_last);
    assert(global_os_argv);

    char *p = global_os_argv[0];
    strncpy(p, title, os_argv_last - p);
    p += strlen(p);

    /* NUL-fill the remainder of the original argv region */
    if (os_argv_last - p) {
        memset(p, SETPROCTITLE_PAD, os_argv_last - p);
    }
}
#endif
