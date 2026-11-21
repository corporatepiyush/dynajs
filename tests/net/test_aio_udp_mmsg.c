/* The epoll reactor drains a UDP socket with recvmmsg, 16 datagrams per call, instead of one
 * recvfrom per datagram (Linux; other kernels keep the recvfrom loop). A DNS-style burst cost one
 * syscall per datagram plus the EAGAIN that ends the drain (1.26 per datagram at depth 32); now a full
 * batch of 64 is four calls. The slots are 64 KiB each, so no datagram is ever truncated, and the
 * per-wake budget (AIO_DGRAM_BATCH) is unchanged, so a flood still yields to timers (test_udp_budget).
 * Pinned here: 64 queued datagrams of mixed sizes (1 byte to 60000) arrive whole, in order, with the
 * sender's address; on Linux the receive syscalls for them stay <= 8 (one per datagram before); a
 * handler that closes the socket mid-batch and opens a new one on the same fd number must not see the
 * rest of the old socket's batch (the batch is already out of the kernel, so the reactor checks the
 * fd's life counter after every callback). Without that check the rest of the batch goes on to the
 * closed socket's own handler (its callback was read before the batch), so both handlers count.
 * A short batch (fewer than asked) does not end the drain: datagrams that arrive while the handlers
 * run are taken by the next call in the same wake, as the recvfrom loop always did; ending early cost a
 * poll round trip per few datagrams (9 per wake instead of 45 with 16 senders, the losing row below).
 * The same-drain check is Linux-only: macOS delivers loopback UDP asynchronously, so datagrams sent inside the
 * handler can land after the recvfrom loop's EAGAIN (7 of 10 runs saw 3 or 4 of 8 in one wake there).
 * The slots are set up once per wake; only msg_namelen (which the kernel overwrites) is reset per call.
 * Measured (Linux, 16 interleaved reps, each started only when host load < 12, with an identical-binary
 * control row; median ratio to base, control in brackets): 16 senders x depth 1 0.980 (1.004, quartiles
 * 0.923-1.027, so inside the control spread; datagrams per wake 52 vs base 54, the ending-on-short-batch
 * version took 9 and lost 0.958); 4x1 1.009 (0.883); 64x1 1.144 (1.068); 1 sender x depth 32 1.455 (0.926).
 * Under strace the receive calls fell from 1.151 per datagram to 0.094.
 * Compiled with dyna-aio.c included so the receive calls can be counted. */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int n_recv_calls;

static ssize_t counted_recvfrom(int fd, void* b, size_t n, int fl, struct sockaddr* sa, socklen_t* sl)
{
    n_recv_calls++;
    return recvfrom(fd, b, n, fl, sa, sl);
}
#define recvfrom counted_recvfrom

#ifdef __linux__
static int counted_recvmmsg(int fd, struct mmsghdr* v, unsigned n, int fl, struct timespec* t)
{
    n_recv_calls++;
    return recvmmsg(fd, v, n, fl, t);
}
#define recvmmsg counted_recvmmsg
#endif

#include "dyna-aio.c"

static int fails;
#define CHECK(c, ...)            \
    do {                         \
        if (!(c)) {              \
            printf("FAIL: ");    \
            printf(__VA_ARGS__); \
            printf("\n");        \
            fails++;             \
        }                        \
    } while (0)

#define NDG 64
static size_t dg_len(int i) { return i == 7 ? 60000 : (size_t)(1 + (i * 37) % 1400); }
static int got, bad, port_bad, after_close;
static uint16_t want_port;
static int close_at = -1, closed_fd = -1, g_rfd = -1, new_fd = -1;

static void on_stale(dyn_aio_t* a, int res, const uint8_t* buf, unsigned n,
    const struct sockaddr* sa, unsigned salen, void* ud)
{
    (void)a;
    (void)res;
    (void)buf;
    (void)n;
    (void)sa;
    (void)salen;
    (void)ud;
    after_close++;
}

#define LATE_FIRST 3
#define LATE_MORE 5
static int late_got, late_sfd = -1;
static struct sockaddr_in late_to;

static void on_late(dyn_aio_t* a, int res, const uint8_t* buf, unsigned n,
    const struct sockaddr* sa, unsigned salen, void* ud)
{
    int i;
    (void)a;
    (void)buf;
    (void)n;
    (void)sa;
    (void)salen;
    (void)ud;
    if (res < 0)
        return;
    if (++late_got == 1)
        for (i = 0; i < LATE_MORE; i++)
            sendto(late_sfd, "m", 1, 0, (const struct sockaddr*)&late_to, sizeof(late_to));
}

static void on_dgram(dyn_aio_t* a, int res, const uint8_t* buf, unsigned n,
    const struct sockaddr* sa, unsigned salen, void* ud)
{
    size_t k;
    (void)salen;
    (void)ud;
    if (closed_fd >= 0) {
        after_close++;
        return;
    }
    if (res < 0)
        return;
    if (n != dg_len(got))
        bad++;
    else
        for (k = 0; k < n; k++)
            if (buf[k] != (uint8_t)(got * 13 + k)) {
                bad++;
                break;
            }
    if (sa->sa_family != AF_INET || ntohs(((const struct sockaddr_in*)sa)->sin_port) != want_port)
        port_bad++;
    got++;
    if (got == close_at) {
        closed_fd = g_rfd;
        dyn_aio_close(a, g_rfd);
        new_fd = dyn_aio_udp_bind(a, "127.0.0.1", 0);
        if (new_fd >= 0)
            dyn_aio_recvfrom(a, new_fd, on_stale, NULL);
    }
}

static int run_burst(dyn_aio_t* a, int rfd, int sfd, const struct sockaddr_in* to)
{
    static uint8_t m[60000];
    int i, k;
    for (i = 0; i < NDG; i++) {
        size_t j, n = dg_len(i);
        for (j = 0; j < n; j++)
            m[j] = (uint8_t)(i * 13 + j);
        if (sendto(sfd, m, n, 0, (const struct sockaddr*)to, sizeof(*to)) != (ssize_t)n)
            return -1;
    }
    (void)rfd;
    for (k = 0; k < 50 && got < NDG && closed_fd < 0; k++)
        dyn_aio_run(a, 100);
    return 0;
}

int main(void)
{
    dyn_aio_t* a = dyn_aio_new(0, 0);
    struct sockaddr_in to, me;
    socklen_t sl = sizeof(to);
    int rfd, sfd, rcv = 4 << 20;

    setvbuf(stdout, NULL, _IOLBF, 0);
    CHECK(a != NULL, "dyn_aio_new");
    if (!a)
        return 1;
    rfd = g_rfd = dyn_aio_udp_bind(a, "127.0.0.1", 0);
    CHECK(rfd >= 0, "udp bind");
    setsockopt(rfd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    getsockname(rfd, (struct sockaddr*)&to, &sl);
    sfd = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &rcv, sizeof(rcv));
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(sfd, (struct sockaddr*)&me, sizeof(me)) == 0, "sender bind");
    sl = sizeof(me);
    getsockname(sfd, (struct sockaddr*)&me, &sl);
    want_port = ntohs(me.sin_port);
    CHECK(dyn_aio_recvfrom(a, rfd, on_dgram, NULL) == 0, "dyn_aio_recvfrom");

    n_recv_calls = 0;
    CHECK(run_burst(a, rfd, sfd, &to) == 0, "send burst");
    CHECK(got == NDG, "%d of %d datagrams arrived", got, NDG);
    CHECK(bad == 0, "%d datagrams differ in length or bytes (the 60000-byte one must not be truncated)", bad);
    CHECK(port_bad == 0, "%d datagrams carried a wrong sender address", port_bad);
#ifdef __linux__
    CHECK(n_recv_calls <= 8, "%d receive syscalls for %d datagrams (want <= 8: batches of 16, not one each)",
        n_recv_calls, NDG);
#endif

    got = 0;
    close_at = 5;
    CHECK(run_burst(a, rfd, sfd, &to) == 0, "send burst 2");
    CHECK(closed_fd >= 0, "the handler saw datagram %d", close_at);
    printf("test_aio_udp_mmsg: closed fd %d mid-batch, the new socket got fd %d\n", closed_fd, new_fd);
    CHECK(after_close == 0, "%d datagrams of the closed socket's batch were delivered after its close",
        after_close);
    printf("test_aio_udp_mmsg: %d receive calls for %d datagrams\n", n_recv_calls, NDG);

    {
        socklen_t ll = sizeof(late_to);
        int lfd = dyn_aio_udp_bind(a, "127.0.0.1", 0), i;
        CHECK(lfd >= 0, "late udp bind");
        getsockname(lfd, (struct sockaddr*)&late_to, &ll);
        late_sfd = sfd;
        CHECK(dyn_aio_recvfrom(a, lfd, on_late, NULL) == 0, "late recvfrom");
        for (i = 0; i < LATE_FIRST; i++)
            sendto(sfd, "f", 1, 0, (const struct sockaddr*)&late_to, sizeof(late_to));
        dyn_aio_run(a, 1000);
#ifdef __linux__
        CHECK(late_got == LATE_FIRST + LATE_MORE,
            "one wake delivered %d of %d datagrams: %d arrived while the handler ran and must be taken in the "
            "same drain, not after another poll",
            late_got, LATE_FIRST + LATE_MORE, LATE_MORE);
#endif
        dyn_aio_close(a, lfd);
    }

    if (new_fd >= 0)
        dyn_aio_close(a, new_fd);
    close(sfd);
    dyn_aio_free(a);
    if (fails == 0)
        printf("test_aio_udp_mmsg: all tests passed\n");
    else
        printf("test_aio_udp_mmsg: %d FAILED\n", fails);
    return fails != 0;
}
