#if defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
#include <sanitizer/lsan_interface.h>
#endif

#include "dyna-aio.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int g_res, g_calls;
static int c_res, c_calls;
static int d_calls, d_len, d_peer_ok;
static void on_dgram(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n,
                     const struct sockaddr *peer, unsigned plen, void *ud)
{
    (void)a; (void)buf; (void)ud;
    d_calls++;
    d_len = res;
    d_peer_ok = (peer != NULL && plen >= sizeof(struct sockaddr_in) &&
                 peer->sa_family == AF_INET &&
                 ((const struct sockaddr_in *)peer)->sin_port != 0);
    (void)n;
}
static void on_conn(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n, void *ud)
{ (void)a; (void)buf; (void)n; (void)ud; c_res = res; c_calls++; }
static void on_done(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n,
                    void *ud)
{
    (void)a; (void)buf; (void)n; (void)ud;
    g_res = res;
    g_calls++;
}

static void pump(dyn_aio_t *a, int want, int max_ms)
{
    int waited = 0;
    while (g_calls < want && waited < max_ms) {
        struct pollfd pfd;
        int fd = dyn_aio_disk_fd(a);
        if (fd < 0) {
            if (dyn_aio_run(a, 10) <= 0) waited += 10;
            continue;
        }
        pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
        if (poll(&pfd, 1, 10) > 0) dyn_aio_disk_drain(a);
        else waited += 10;
    }
}

int main(void)
{
    char path[] = "/tmp/dyn_aio_disk_XXXXXX";
    dyn_aio_t *a;
    int tmpfd;
    char rbuf[64];
    const char *msg = "hello disk";

    setvbuf(stdout, NULL, _IOLBF, 0);

    tmpfd = mkstemp(path);
    CHECK(tmpfd >= 0, "mkstemp");
    if (tmpfd < 0) return 1;

    a = dyn_aio_new(64, 0);
    CHECK(a != NULL, "dyn_aio_new");
    if (!a) return 1;

    g_calls = 0; g_res = -1;
    CHECK(dyn_aio_write(a, tmpfd, msg, strlen(msg), 0, on_done, NULL) == 0,
          "dyn_aio_write must not return ENOSYS any more");
    pump(a, 1, 5000);
    CHECK(g_calls == 1, "write completed %d times, want 1", g_calls);
    CHECK(g_res == (int)strlen(msg), "write returned %d, want %zu",
          g_res, strlen(msg));

    g_calls = 0; g_res = -1;
    CHECK(dyn_aio_fsync(a, tmpfd, 1, on_done, NULL) == 0, "dyn_aio_fsync");
    pump(a, 1, 5000);
    CHECK(g_calls == 1 && g_res == 0, "fsync res %d (calls %d), want 0",
          g_res, g_calls);

    g_calls = 0; g_res = -1;
    memset(rbuf, 0, sizeof(rbuf));
    CHECK(dyn_aio_read(a, tmpfd, rbuf, sizeof(rbuf), 0, on_done, NULL) == 0,
          "dyn_aio_read");
    pump(a, 1, 5000);
    CHECK(g_res == (int)strlen(msg), "read returned %d, want %zu",
          g_res, strlen(msg));
    CHECK(memcmp(rbuf, msg, strlen(msg)) == 0,
          "read gave '%.10s', want '%s' -- a byte count alone would not catch this",
          rbuf, msg);

    g_calls = 0; g_res = 1;
    CHECK(dyn_aio_openat(a, AT_FDCWD, path, O_RDONLY, 0, on_done, NULL) == 0,
          "dyn_aio_openat");
    pump(a, 1, 5000);
    CHECK(g_res >= 0, "openat returned %d, want a fd", g_res);
    if (g_res >= 0) close(g_res);

    g_calls = 0; g_res = 0;
    CHECK(dyn_aio_openat(a, AT_FDCWD, "/nonexistent/dyn/x", O_RDONLY, 0,
                         on_done, NULL) == 0, "openat (missing) submits");
    pump(a, 1, 5000);
    CHECK(g_res == -ENOENT, "missing file gave %d, want -ENOENT (%d)",
          g_res, -ENOENT);

    g_calls = 0;
    {
        int i, submitted = 0;
        for (i = 0; i < 64; i++)
            if (dyn_aio_write(a, tmpfd, msg, strlen(msg), 0, on_done, NULL) == 0)
                submitted++;
        pump(a, submitted, 10000);
        CHECK(g_calls == submitted,
              "%d of %d completions arrived; none may be lost when the pool "
              "refuses and the op runs inline", g_calls, submitted);
    }

    {
        int lfd = dyn_aio_listen(a, "127.0.0.1", 0, 16);
        CHECK(lfd >= 0, "dyn_aio_listen for the cancel case");
        if (lfd >= 0) {
            int cancelled;
            g_calls = 0;
            CHECK(dyn_aio_accept(a, lfd, on_done, (void *)0x1234) == 0,
                  "arm a multishot accept");
            cancelled = dyn_aio_cancel(a, on_done, (void *)0x1234);
            CHECK(cancelled >= 1,
                  "cancel must report the op it disarmed (got %d)", cancelled);
            CHECK(dyn_aio_cancel(a, on_done, (void *)0x1234) == 0,
                  "a second cancel has nothing left to disarm");
            CHECK(dyn_aio_cancel(a, on_done, (void *)0xDEAD) == 0,
                  "cancelling an unknown cookie must not claim a victim");
            dyn_aio_run(a, 20);
            CHECK(g_calls == 0,
                  "a cancelled op fired %d callbacks; it must fire none",
                  g_calls);
            dyn_aio_close(a, lfd);
        }
    }

    {
        int woke;
        g_calls = 0;
        CHECK(dyn_aio_write(a, tmpfd, msg, strlen(msg), 0, on_done, NULL) == 0,
              "arm a disk op with nothing else pending");
        woke = dyn_aio_run(a, 4000);
        CHECK(woke > 0 || g_calls > 0,
              "a lone disk completion must wake the reactor (dispatched %d, "
              "callbacks %d) -- otherwise the loop sleeps through it", woke,
              g_calls);
        pump(a, 1, 4000);
        CHECK(g_calls == 1, "the disk callback ran %d times, want 1", g_calls);
    }

    {
        int lfd = dyn_aio_listen(a, "127.0.0.1", 0, 16);
        CHECK(lfd >= 0, "listen for the connect case");
        if (lfd >= 0) {
            struct sockaddr_in sa;
            socklen_t sl = sizeof(sa);
            int cfd;
            CHECK(getsockname(lfd, (struct sockaddr *)&sa, &sl) == 0, "getsockname");
            dyn_aio_accept(a, lfd, on_done, (void *)0x55);

            c_calls = 0; c_res = -12345;
            cfd = dyn_aio_connect(a, "127.0.0.1", ntohs(sa.sin_port),
                                  on_conn, (void *)0x66);
            CHECK(cfd >= 0, "connect returns a fd immediately (got %d)", cfd);
            { int spins = 0; while (c_calls == 0 && spins++ < 400) dyn_aio_run(a, 10); }
            CHECK(c_res == 0, "connect to a listening port must report 0, got %d", c_res);
            if (cfd >= 0) dyn_aio_close(a, cfd);

            c_calls = 0; c_res = -12345;
            cfd = dyn_aio_connect(a, "127.0.0.1", 1, on_conn, (void *)0x77);
            if (cfd >= 0) {
                int spins = 0;
                while (c_calls == 0 && spins++ < 400) dyn_aio_run(a, 10);
                CHECK(c_calls == 1, "a refused connect must still complete");
                CHECK(c_res < 0,
                      "a REFUSED connect reported %d; writability alone is not "
                      "success -- SO_ERROR must be checked", c_res);
                dyn_aio_close(a, cfd);
            }
            dyn_aio_cancel(a, on_done, (void *)0x55);
            dyn_aio_close(a, lfd);
        }
    }

    {
        int sfd = dyn_aio_udp_bind(a, "127.0.0.1", 0);
        CHECK(sfd >= 0, "dyn_aio_udp_bind");
        if (sfd >= 0) {
            struct sockaddr_in sa, peer;
            socklen_t sl = sizeof(sa);
            int cfd;
            CHECK(getsockname(sfd, (struct sockaddr *)&sa, &sl) == 0, "udp getsockname");
            CHECK(dyn_aio_recvfrom(a, sfd, on_dgram, (void *)0x99) == 0,
                  "dyn_aio_recvfrom must not be ENOSYS");

            cfd = dyn_aio_udp_bind(a, "127.0.0.1", 0);
            memset(&peer, 0, sizeof(peer));
            peer.sin_family = AF_INET;
            peer.sin_port = sa.sin_port;
            peer.sin_addr.s_addr = inet_addr("127.0.0.1");

            d_calls = 0; d_len = -1; d_peer_ok = 0;
            CHECK(dyn_aio_sendto(a, cfd, "ping", 4, (struct sockaddr *)&peer,
                                 sizeof(peer)) == 4, "sendto returned the length");
            { int sp = 0; while (d_calls == 0 && sp++ < 400) dyn_aio_run(a, 10); }
            CHECK(d_calls == 1, "one datagram delivered, got %d", d_calls);
            CHECK(d_len == 4, "payload length %d, want 4", d_len);
            CHECK(d_peer_ok, "the PEER ADDRESS must arrive -- without it a "
                             "datagram server cannot reply");

            d_calls = 0; d_len = -1;
            CHECK(dyn_aio_sendto(a, cfd, "", 0, (struct sockaddr *)&peer,
                                 sizeof(peer)) == 0, "zero-length sendto");
            { int sp = 0; while (d_calls == 0 && sp++ < 400) dyn_aio_run(a, 10); }
            CHECK(d_calls == 1 && d_len == 0,
                  "a ZERO-LENGTH datagram is legal and must be delivered "
                  "(calls=%d len=%d)", d_calls, d_len);
            if (cfd >= 0) dyn_aio_close(a, cfd);
            dyn_aio_close(a, sfd);
        }
    }

    {
        char sp[] = "/tmp/dyn_ipc_XXXXXX";
        int lfd, cfd;
        char toolong[512];

        int tfd = mkstemp(sp);
        if (tfd >= 0) close(tfd);

        lfd = dyn_aio_unix_listen(a, sp, 8);
        CHECK(lfd >= 0, "unix listen on %s", sp);
        if (lfd >= 0) {
            g_calls = 0;
            dyn_aio_accept(a, lfd, on_done, (void *)0xAA);
            c_calls = 0; c_res = -12345;
            cfd = dyn_aio_unix_connect(a, sp, on_conn, (void *)0xBB);
            CHECK(cfd >= 0, "unix connect returns a fd (got %d)", cfd);
            { int sp2 = 0; while (c_calls == 0 && sp2++ < 400) dyn_aio_run(a, 10); }
            CHECK(c_res == 0, "unix connect reported %d, want 0", c_res);
            { int sp2 = 0; while (g_calls == 0 && sp2++ < 200) dyn_aio_run(a, 10); }
            CHECK(g_calls >= 1, "the listener must accept the unix connection");
            if (cfd >= 0) dyn_aio_close(a, cfd);
            dyn_aio_cancel(a, on_done, (void *)0xAA);
            dyn_aio_close(a, lfd);
        }
        unlink(sp);

        memset(toolong, 'x', sizeof(toolong) - 1);
        toolong[sizeof(toolong) - 1] = 0;
        memcpy(toolong, "/tmp/dyn_ipc_trunc_", 19);
        CHECK(dyn_aio_unix_listen(a, toolong, 4) < 0,
              "an over-long unix path must be REFUSED, not truncated to a "
              "different (and bindable) path");
        CHECK(dyn_aio_unix_connect(a, toolong, on_conn, NULL) < 0,
              "an over-long unix path must be refused on connect too");
        { char t2[128]; memcpy(t2, toolong, 100); t2[100] = 0; unlink(t2); }
    }

    {
        int lfd = dyn_aio_listen(a, "127.0.0.1", 0, 8);
        if (lfd >= 0) {
            struct sockaddr_in sa; socklen_t sl = sizeof(sa);
            int cfd;
            getsockname(lfd, (struct sockaddr *)&sa, &sl);
            dyn_aio_accept(a, lfd, on_done, (void *)0x71);
            c_calls = 0;
            cfd = dyn_aio_connect(a, "127.0.0.1", ntohs(sa.sin_port),
                                  on_conn, (void *)0x72);
            { int sp = 0; while (c_calls == 0 && sp++ < 400) dyn_aio_run(a, 10); }
            if (cfd >= 0 && c_res == 0) {
                size_t big = 1u << 20;
                uint8_t *heap = (uint8_t *)malloc(big);
                CHECK(heap != NULL, "alloc for the ownership case");
                if (heap) {
                    memset(heap, 0xA5, big);
                    CHECK(dyn_aio_send(a, cfd, heap, big, 0, NULL, NULL) == 0,
                          "queue a send larger than the socket buffer");
                    free(heap);
                    { int sp = 0; while (sp++ < 40) dyn_aio_run(a, 5); }
                    printf("  send ownership: caller freed, no ASan report\n");
                }
                dyn_aio_close(a, cfd);
            }
            dyn_aio_cancel(a, on_done, (void *)0x71);
            dyn_aio_close(a, lfd);
        }
    }

    {
        int lfd = dyn_aio_listen(a, "127.0.0.1", 0, 4);
        if (lfd >= 0) {
            CHECK(dyn_aio_recv(a, lfd, 0, 1, NULL, NULL) == -1,
                  "dyn_aio_recv must REFUSE a NULL callback");
            CHECK(errno == EINVAL, "and say EINVAL, not a stale errno (got %d)",
                  errno);
            CHECK(dyn_aio_recv(a, lfd, 0, 1, on_done, (void *)0x91) == 0,
                  "and a real callback still arms on the same fd");
            dyn_aio_cancel(a, on_done, (void *)0x91);
            dyn_aio_close(a, lfd);
        }
    }

    g_calls = 0;
#if defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
    __lsan_disable();
#endif
    (void)dyn_aio_write(a, tmpfd, msg, strlen(msg), 0, on_done, NULL);
    dyn_aio_free(a);
#if defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
    __lsan_enable();
#endif
    printf("  teardown with disk in flight: ok\n");

    close(tmpfd);
    unlink(path);
    if (fails == 0) printf("test_aio_disk: all tests passed\n");
    else printf("test_aio_disk: %d FAILED\n", fails);
    return fails != 0;
}
