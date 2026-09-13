/* test_socket_cloexec.c -- engine-created sockets must be close-on-exec.
 *
 * PROBLEM: os.exec() forks; without FD_CLOEXEC on the engine's sockets, a
 * spawned child inherits every listener/client fd the process happens to hold.
 * (dyn_exec_child also closes fds 3..maxfd before execve as a second barrier,
 * but that is defense-in-depth in the exec path, NOT a property of the socket
 * itself -- and it is not portable to callers that hand a raw fd to a C
 * library or spawn via a non-dyn exec. CLOEXEC is the correct, OS-guaranteed
 * property, and it is what the audit asks for.)
 *
 * WHY NOT the JS fd-count proof: dyn_exec_child (dyna-proc.inc.c) closes
 * every fd >= 3 before execve, so a child counting /dev/fd sees the same small
 * number whether or not the parent's sockets are CLOEXEC. The count cannot
 * discriminate; only inspecting the flag directly can.
 *
 * We exercise the SAME call shape TCPServer uses -- dyn_aio_listen() -- and
 * assert F_GETFD carries FD_CLOEXEC. This is host-independent (the socket()
 * call is explicit, not a getaddrinfo result) and deterministic.
 *
 * Build and run (macOS here; Linux identical):
 *   $(CC) -O1 -g $(CFLAGS) -Isrc -o test_socket_cloexec \
 *       tests/test_socket_cloexec.c $(NAT_MODULE_OBJS) libdynajs.a \
 *       $(LDFLAGS) $(LIBS) $(SQLITE_LIBS)
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include "dyna-aio.h"

int main(void)
{
    dyn_aio_t *a;
    int fd, flags, rc = 0;

    a = dyn_aio_new(0, 0);
    if (!a) { printf("FAIL: dyn_aio_new returned NULL\n"); return 1; }

    /* dyn_aio_listen is what TCPServer binds with; it opens a SOCK_STREAM
       socket in the engine. Port 0 = kernel-assigned. The daemon never needs
       to actually serve -- just to hold the fd so its flag can be checked. */
    fd = dyn_aio_listen(a, "127.0.0.1", 0, 8);
    if (fd < 0) {
        printf("FAIL: dyn_aio_listen (%s)\n", "see errno");
        dyn_aio_free(a);
        return 1;
    }

    flags = fcntl(fd, F_GETFD);
    if (flags < 0) { printf("FAIL: fcntl F_GETFD\n"); rc = 1; }
    else if (!(flags & FD_CLOEXEC)) {
        printf("FAIL: engine socket fd %d is NOT FD_CLOEXEC\n", fd);
        rc = 1;
    }
    else
        printf("OK: engine socket fd %d is FD_CLOEXEC\n", fd);

    close(fd);
    dyn_aio_free(a);
    return rc;
}
