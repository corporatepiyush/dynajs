#include "http-static-open.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

dyn_static_root_t* dyn_static_root_new(const char* dir)
{
    dyn_static_root_t* r = (dyn_static_root_t*)calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->dir = strdup(dir);
    if (!r->dir) {
        free(r);
        return NULL;
    }
    return r;
}

void dyn_static_root_free(dyn_static_root_t* r)
{
    if (!r)
        return;
    free(r->dir);
    free(r);
}

int dyn_static_open(dyn_static_root_t* r, const char* rel, int* status)
{
    char fpath[2048], rdir[PATH_MAX], rfile[PATH_MAX];
    size_t dl;
    int fd;
    if (snprintf(fpath, sizeof(fpath), "%s/%s", r->dir, rel) >= (int)sizeof(fpath)) {
        *status = 404;
        return -1;
    }
    if (!realpath(r->dir, rdir) || !realpath(fpath, rfile)) {
        *status = 404;
        return -1;
    }
    dl = strlen(rdir);
    if (strncmp(rfile, rdir, dl) != 0 || (rfile[dl] != '/' && rfile[dl] != '\0')) {
        *status = 403;
        return -1;
    }
    fd = open(rfile, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        *status = errno == ELOOP ? 403 : (errno == ENOENT ? 404 : 500);
        return -1;
    }
    return fd;
}
#endif
