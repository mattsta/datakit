#pragma once

/* This is modified from nginx/src/os/unix/ngx_setproctitle.h (BSD-2) */

#include <stdbool.h>

#if DK_OS_FREEBSD || DK_OS_NETBSD || DK_OS_OPENBSD
#define setProctitleInit() true
#define setproctitle(title) setproctitle("%s", title)
#else
#if DK_OS_SOLARIS || DK_OS_LINUX || DK_OS_APPLE
bool setProctitleInit(char *argv[]);
void setproctitle(char *title);
#else
#define setProctitleInit() true
#define setproctitle(title)
#endif
#endif
