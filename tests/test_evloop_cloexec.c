/* test_evloop_cloexec.c -- the evloop backend descriptor must be close-on-exec.
 *
 * Build and run (Linux is the platform that matters -- see below):
 *   clang -O1 -Isrc -DCONFIG_NATIVE_MODULES -o /tmp/t \
 *         tests/test_evloop_cloexec.c src/dyna-evloop.c && /tmp/t
 *
 * WHY: os.exec() forks. A child that inherits the epoll/kqueue descriptor
 * either leaks it across exec, or -- if it does not exec -- shares the kernel
 * interest set with the parent, so epoll_ctl in one process silently perturbs
 * the other.
 *
 * THIS TEST IS VACUOUS ON macOS. kqueue() there already returns a descriptor
 * with FD_CLOEXEC set, so it passes with or without the fix. It was verified to
 * FAIL on linux/amd64 against the pre-fix code (`epoll_create1(0)`), which is
 * the only configuration where the bug was reachable -- and the reason it went
 * unnoticed on an arm64 dev host. Run it in docker:
 *   docker build --platform linux/amd64 ...   (see bench/EVLOOP_FINDINGS.md)
 */
/* Proves the evloop backend descriptor is close-on-exec. Without it, os.exec()
   leaks the epoll/kqueue fd into the child, and a forked child that does not
   exec shares the interest set with the parent. */
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
