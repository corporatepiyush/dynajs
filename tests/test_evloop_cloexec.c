#include <stdio.h>
#include <fcntl.h>
#include "dyna-evloop.h"
int main(void) {
    dyn_evloop_t *lp = dyn_evloop_new();
    int fd, flags, rc = 0;
    if (!lp) { printf("FAIL: dyn_evloop_new returned NULL\n"); return 1; }
    fd = dyn_evloop_backend_fd(lp);
    if (fd < 0) { printf("SKIP: poll(2) backend has no descriptor\n"); dyn_evloop_free(lp); return 0; }
    flags = fcntl(fd, F_GETFD);
    if (flags < 0) { printf("FAIL: fcntl F_GETFD\n"); rc = 1; }
    else if (!(flags & FD_CLOEXEC)) { printf("FAIL: backend fd %d is NOT FD_CLOEXEC\n", fd); rc = 1; }
    else printf("OK: backend fd %d is FD_CLOEXEC\n", fd);
    dyn_evloop_free(lp);
    return rc;
}
