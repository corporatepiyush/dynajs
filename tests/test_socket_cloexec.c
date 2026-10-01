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
