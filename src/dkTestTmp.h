#pragma once

/* Per-run scratch directories for tests.
 *
 * A test that needs files on disk declares a scope-owned directory and
 * asks it for paths inside it:
 *
 *     cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
 *     char base[PATH_MAX];
 *     if (!dkTestTmpDirPath(&tmp, "flexPTest", base, sizeof(base))) {
 *         ...fail the test...
 *     }
 *
 * The first dkTestTmpDirPath call creates a fresh directory with
 * mkdtemp under $TMPDIR (or the platform's P_tmpdir when $TMPDIR is
 * unset), so concurrent test processes never share a file.  The
 * cleanup removes the directory and everything beneath it at scope
 * exit, early returns included.  A test that wants to assert the
 * removal itself calls dkTestTmpDirRemove directly; the scope cleanup
 * then has nothing left to do. */

#include "cleaner.h"
#include "datakit.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct dkTestTmpDir {
    char path[PATH_MAX]; /* empty until the directory exists */
} dkTestTmpDir;

/* The directory new scratch directories are created in: $TMPDIR when
 * set and non-empty, else the platform default. */
DK_FN_UNUSED static const char *dkTestTmpRoot(void) {
    const char *root = getenv("TMPDIR");
    if (root && *root) {
        return root;
    }

#ifdef P_tmpdir
    return P_tmpdir;
#else
    return "/tmp";
#endif
}

/* Create the directory if it does not exist yet.  Returns false (with
 * errno set) when the path does not fit or mkdtemp fails. */
DK_FN_UNUSED static bool dkTestTmpDirCreate(dkTestTmpDir *d) {
    if (d->path[0]) {
        return true;
    }

    const char *root = dkTestTmpRoot();
    size_t rootLen = strlen(root);
    while (rootLen > 1 && root[rootLen - 1] == '/') {
        rootLen--;
    }

    char tmpl[PATH_MAX];
    const int n =
        snprintf(tmpl, sizeof(tmpl), "%.*s/dkTest.XXXXXX", (int)rootLen, root);
    if (n < 0 || (size_t)n >= sizeof(tmpl)) {
        errno = ENAMETOOLONG;
        return false;
    }

    if (!mkdtemp(tmpl)) {
        return false;
    }

    memcpy(d->path, tmpl, (size_t)n + 1);
    return true;
}

/* Write "<dir>/<name>" into out, creating the directory on first use.
 * Returns false when the directory cannot be created or the joined
 * path does not fit in outLen. */
DK_FN_UNUSED static bool dkTestTmpDirPath(dkTestTmpDir *d, const char *name,
                                          char *out, size_t outLen) {
    if (!dkTestTmpDirCreate(d)) {
        return false;
    }

    const int n = snprintf(out, outLen, "%s/%s", d->path, name);
    if (n < 0 || (size_t)n >= outLen) {
        errno = ENAMETOOLONG;
        return false;
    }

    return true;
}

DK_FN_UNUSED static void dkTestTmpCloseDir_cleanup(DIR **dir) {
    if (*dir) {
        closedir(*dir);
    }
}

/* Remove path and, when it is a directory, everything beneath it.
 * Symlinks are removed, never followed.  Each directory is made
 * owner-accessible before it is emptied, so a test that left one
 * read-only still cleans up.  Entries removed during a readdir pass
 * may make some filesystems skip others, so a directory is rescanned
 * until a pass finds nothing left or removes nothing. */
DK_FN_UNUSED static bool dkTestTmpRemoveTree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return errno == ENOENT;
    }

    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 || errno == ENOENT;
    }

    (void)chmod(path, S_IRWXU);

    bool ok = true;
    {
        cleaner(dkTestTmpCloseDir) DIR *dir = opendir(path);
        if (!dir) {
            return false;
        }

        for (;;) {
            uint64_t seen = 0;
            uint64_t removed = 0;
            rewinddir(dir);
            const struct dirent *ent;
            while ((ent = readdir(dir))) {
                if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
                    continue;
                }

                seen++;
                char child[PATH_MAX];
                const int n =
                    snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
                if (n >= 0 && (size_t)n < sizeof(child) &&
                    dkTestTmpRemoveTree(child)) {
                    removed++;
                }
            }

            if (seen == 0) {
                break;
            }

            if (removed == 0) {
                ok = false;
                break;
            }
        }
    }

    return rmdir(path) == 0 && ok;
}

/* Remove the directory and its contents, then mark it empty so a later
 * call (or the scope cleanup) does nothing.  Returns true when nothing
 * is left on disk. */
DK_FN_UNUSED static bool dkTestTmpDirRemove(dkTestTmpDir *d) {
    if (!d->path[0]) {
        return true;
    }

    const bool ok = dkTestTmpRemoveTree(d->path);
    d->path[0] = '\0';
    return ok;
}

DK_FN_UNUSED static void dkTestTmpDirRemove_cleanup(dkTestTmpDir *d) {
    (void)dkTestTmpDirRemove(d);
}
