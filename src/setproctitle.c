/* This is modified from nginx/src/os/unix/setproctitle.c (BSD-2) */

#include "datakit.h"
#include <assert.h>

#if DK_OS_LINUX || DK_OS_APPLE || DK_OS_SOLARIS
#if DK_OS_SOLARIS
#define SETPROCTITLE_PAD ' '
#else
#define SETPROCTITLE_PAD '\0'
#endif

/* Original comment from nginx */
/*
 * To change the process title in Linux and Solaris we have to set argv[1]
 * to NULL and to copy the title to the same place where the argv[0] points to.
 * However, argv[0] may be too small to hold a new title.  Fortunately, Linux
 * and Solaris store argv[] and environ[] one after another.  So we should
 * ensure that is the continuous memory and then we allocate the new memory
 * for environ[] and copy it.  After this we could use the memory starting
 * from argv[0] for our process title.
 *
 * The Solaris's standard /bin/ps does not show the changed process title.
 * You have to use "/usr/ucb/ps -w" instead.  Besides, the UCB ps does not
 * show a new title if its length less than the origin command line length.
 * To avoid it we append to a new title the origin command line in the
 * parenthesis.
 */

extern char **environ;

static const char *os_argv_last = NULL;
static char **global_os_argv = NULL;

bool setProctitleInit(char *os_argv[]) {
    size_t size = 0;

    /* We need to save argv[] itself for setProctitle() later. */
    global_os_argv = os_argv;

    for (size_t i = 0; environ[i]; i++) {
        size += strlen(environ[i]) + 1;
    }

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

            strncpy(p, environ[i], size);
            environ[i] = (char *)p;
            p += size;
        }
    }

    os_argv_last--;
    os_argv[1] = NULL;

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

#if DK_OS_SOLARIS
    size_t size = 0;

    for (int32_t i = 0; i < argc; i++) {
        size += strlen(argv[i]) + 1;
    }

    if (size > (size_t)((char *)p - os_argv[0])) {
        strncpy(p, (uint8_t *)" (", os_argv_last - (char *)p);
        p += strlen(p);

        for (i = 0; i < argc; i++) {
            strncpy(p, (uint8_t *)argv[i], os_argv_last - (char *)p);
            p += strlen(p);
            strncpy(p, (uint8_t *)" ", os_argv_last - (char *)p);
            p += strlen(p);
        }

        if (*(p - 1) == ' ') {
            *(p - 1) = ')';
        }
    }
#endif

    /* Terminate as appropriate (null on darwin/linux, spaces on solaris) */
    if (os_argv_last - p) {
        memset(p, SETPROCTITLE_PAD, os_argv_last - p);
    }
}
#endif
