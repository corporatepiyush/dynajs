/* test_aio_connect_offload.c -- dyn_aio_connect's hostname path must resolve
 * OFF the caller's thread (audit E11-04) while the numeric path keeps the
 * synchronous fd-return contract, and a name that cannot resolve must still
 * produce a completion ("always calls back").
 *
 * WHY THE FD IS STILL SYNCHRONOUS FOR HOSTNAMES: every in-tree caller
 * (net-tcp/redis/pg, the App proxy upstream) stores the return value and
 * treats fd < 0 as "failed synchronously, udata freed" -- a hostname path
 * that returned -1-but-called-back-later would hand those callbacks freed
 * memory. So the offloaded path hands out a DUAL-STACK socket (AF_INET6,
 * V6ONLY=0) before the family is known and connects it to a v4-mapped
 * address when the resolver answers AF_INET. The returned fd is therefore
 * real, recvable and closeable on every path, and the resolve still never
 * runs on the loop thread.
 *
 * HOW THE OFFLOAD IS PROVEN, not assumed: the main thread only ever calls
 * dyn_aio_run(). For the hostname path the lookup runs on a pool worker and
 * its done() is delivered solely through the pool channel's wake fd, which
 * dyn_aio_offload registers inside this reactor -- if that machinery were
 * broken, cb1 below would never fire and case (a) would fail at the pump
 * cap. Case (b) is the control: the numeric path arms a family-true socket
 * directly, same pump, same delivery.
 *
 * Listeners sit on BOTH 127.0.0.1 and ::1: "localhost" may answer in either
 * family depending on the host's resolver order, the OLD code connects a
 * family-true socket to the first answer, and the NEW code drives the same
 * first answer through the dual-stack socket -- every combination lands on
 * one of the two listeners.
 *
 * Case (c) assumes the pool EXISTS (the default build). Under the T1
 * topology (dyn_pool_set_default_threads(0)) the offload runs inline and a
 * failed lookup is a synchronous -1/EINVAL with no callback -- today's
 * behavior, which is exactly what the inline fallback promises to keep.
 *
 * Case (d) pins the SEQUENTIAL RETRY across resolver answers: "localhost"
 * with ::1 sorted FIRST and only a v4 listener bound means the first dial is
 * refused and the SECOND answer must win -- the AF_INET-bound-upstream 502
 * class. It resolves in-process first and SKIPs LOUDLY when the host sorts
 * 127.0.0.1 first (resolver policy, not code).
 *
 * The file is BACKEND-AGNOSTIC (public dyn_aio surface only): the readiness
 * link uses dyna-aio.c; the uring link (make test-uring-connect-offload,
 * docker) compiles the SAME main against dyna-aio-uring.c.
 *
 * Build/run standalone (mirrors `make test-aio-disk`):
 *   cc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
 *      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
 *      tests/test_aio_connect_offload.c src/dyna-aio.c src/dyna-evloop.c \
 *      src/dyna-io.c src/core/dyn-pool.c src/cutils.c -lpthread \
 *      -o /tmp/test_aio_connect_offload
 */
#include "dyna-aio.h"

#if defined(CONFIG_NATIVE_MODULES)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* Pump budget: generous for a resolver round-trip, but BOUNDED so a lost
 * completion fails the test instead of hanging it. */
#define PUMP_MAX 1500

/* One accept recorder shared by both listeners: res IS the accepted fd. */
static int acc_count, acc_fd;

/* Separate connect recorders: which subtest a completion belongs to must not
 * be guessable from a shared counter. */
static int c1_calls, c1_res;    /* (a) hostname */
static int c2_calls, c2_res;    /* (b) numeric */
static int c3_calls, c3_res;    /* (c) unresolvable name */
static int c4_calls, c4_res;    /* (d) sequential retry */

static void on_accept(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n,
                      void *ud)
{ (void)a; (void)buf; (void)n; (void)ud; acc_count++; acc_fd = res; }

static void on_conn1(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n,
                     void *ud)
{ (void)a; (void)buf; (void)n; (void)ud; c1_calls++; c1_res = res; }

static void on_conn2(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n,
                     void *ud)
{ (void)a; (void)buf; (void)n; (void)ud; c2_calls++; c2_res = res; }

static void on_conn3(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n,
                     void *ud)
{ (void)a; (void)buf; (void)n; (void)ud; c3_calls++; c3_res = res; }

static void on_conn4(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n,
                     void *ud)
{ (void)a; (void)buf; (void)n; (void)ud; c4_calls++; c4_res = res; }

/* The single pump: dyn_aio_run only. This is the whole proof of delivery --
 * see the file header. */
static void pump(dyn_aio_t *a, volatile int *counter, int max_spins)
{
    int spins;
    for (spins = 0; *counter == 0 && spins < max_spins; spins++)
        dyn_aio_run(a, 20);
}

/* The connect completion and the listener's accept event are INDEPENDENT
 * reactor events: writability on the client socket is reported without any
 * guarantee the listener's readability lands in the same poll batch, so the
 * peer-side proof gets its own bounded pump rather than sharing the
 * connect's exit condition. */
static void pump_accept(dyn_aio_t *a, int want, int max_spins)
{
    int spins;
    for (spins = 0; acc_count < want && spins < max_spins; spins++)
        dyn_aio_run(a, 10);
}

int main(void)
{
    dyn_aio_t *a;
    int lfd4 = -1, lfd6 = -1, port = 0, cfd, spins;

    setvbuf(stdout, NULL, _IOLBF, 0);

    a = dyn_aio_new(0, 0);
    CHECK(a != NULL, "dyn_aio_new");
    if (!a)
        return 1;

    /* Fixed-port probe: dyn_aio_listen does not report its port, so walk a
     * small range until BOTH stacks bind the SAME port (a half-bound pair
     * cannot prove family-independence of the hostname path). */
    for (port = 47650; port <= 47699; port++) {
        int f4 = dyn_aio_listen(a, "127.0.0.1", (uint16_t)port, 4);
        int f6 = dyn_aio_listen(a, "::1", (uint16_t)port, 4);
        if (f4 >= 0 && f6 >= 0) { lfd4 = f4; lfd6 = f6; break; }
        if (f4 >= 0) dyn_aio_close(a, f4);
        if (f6 >= 0) dyn_aio_close(a, f6);
    }
    CHECK(lfd4 >= 0 && lfd6 >= 0, "no free port in 47650..47699 for both stacks");
    if (lfd4 < 0 || lfd6 < 0) { dyn_aio_free(a); return 1; }
    printf("  listeners: 127.0.0.1:%d and [::1]:%d\n", port, port);

    CHECK(dyn_aio_accept(a, lfd4, on_accept, (void *)0xA4) == 0, "accept v4");
    CHECK(dyn_aio_accept(a, lfd6, on_accept, (void *)0xA6) == 0, "accept v6");

    /* ---- (a) hostname connect: offloaded resolve, fd still returned now ---- */
    c1_calls = 0; c1_res = -12345; acc_count = 0;
    cfd = dyn_aio_connect(a, "localhost", (uint16_t)port, on_conn1, (void *)0xC1);
    CHECK(cfd >= 0,
          "hostname connect must still return an fd the caller can use "
          "(got %d) -- every in-tree caller stores it and closes it", cfd);
    pump(a, &c1_calls, PUMP_MAX);
    CHECK(c1_calls == 1, "hostname connect completed %d times, want 1 "
          "(delivery runs through the pool wake fd; 0 means it never came)",
          c1_calls);
    CHECK(c1_res == 0, "hostname connect res=%d, want 0", c1_res);
    pump_accept(a, 1, 200);
    CHECK(acc_count >= 1, "the peer never saw the connection (accepts=%d)",
          acc_count);
    if (cfd >= 0)
        dyn_aio_close(a, cfd);      /* the fd is the caller's either way */
    if (acc_fd >= 0)
        close(acc_fd);

    /* ---- (b) numeric connect: today's synchronous contract unchanged ------ */
    c2_calls = 0; c2_res = -12345; acc_count = 0; acc_fd = -1;
    cfd = dyn_aio_connect(a, "127.0.0.1", (uint16_t)port, on_conn2, (void *)0xC2);
    CHECK(cfd >= 0, "numeric connect returns an fd immediately (got %d)", cfd);
    pump(a, &c2_calls, PUMP_MAX);
    CHECK(c2_calls == 1, "numeric connect completed %d times, want 1", c2_calls);
    CHECK(c2_res == 0, "numeric connect res=%d, want 0", c2_res);
    pump_accept(a, 1, 200);
    CHECK(acc_count >= 1, "the v4 listener never saw the numeric connect");
    if (cfd >= 0)
        dyn_aio_close(a, cfd);
    if (acc_fd >= 0)
        close(acc_fd);

    /* ---- (c) unresolvable name: the ALWAYS-COMPLETES contract -------------
     * ".test" is reserved by RFC 6761 and never resolves. The hostname path
     * already returned an fd, so the only way this can end is the callback --
     * a dropped completion would strand the caller holding a live socket. */
    c3_calls = 0; c3_res = 0;
    cfd = dyn_aio_connect(a, "invalid.invalid-dns-name.test", (uint16_t)port,
                          on_conn3, (void *)0xC3);
    CHECK(cfd >= 0,
          "an unresolvable hostname still yields the fd first (got %d); the "
          "verdict must come back as the callback", cfd);
    pump(a, &c3_calls, PUMP_MAX);
    CHECK(c3_calls == 1,
          "the failed lookup completed %d times, want 1 -- a hostname connect "
          "must ALWAYS call back once the fd was returned", c3_calls);
    CHECK(c3_res < 0, "failed-lookup res=%d, want a negative -errno", c3_res);
    if (cfd >= 0)
        dyn_aio_close(a, cfd);      /* the caller's failure path closes it */

    /* ---- (d) sequential retry: first answer refused, second answer wins -----
     * Only a v4 listener exists; "localhost" resolving ::1 first (checked --
     * SKIP loudly when the resolver orders differently) means the ::1 dial is
     * refused and the retry must land on 127.0.0.1. Against first-answer-wins
     * code this fails with res=-ECONNREFUSED: the mutation proof. */
    {
        struct addrinfo hints, *res = NULL;
        char portstr[16];
        int lfd4b = -1, port2;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(portstr, sizeof(portstr), "%u", (unsigned)9);
        if (getaddrinfo("localhost", portstr, &hints, &res) != 0 || !res) {
            if (res) freeaddrinfo(res);
            printf("  SKIP (d): localhost does not resolve here\n");
        } else if (res->ai_family != AF_INET6) {
            printf("  SKIP (d): resolver sorts 127.0.0.1 first "
                   "(family %d); the refusal case needs ::1 first\n",
                   res->ai_family);
            freeaddrinfo(res);
        } else {
            freeaddrinfo(res);
            for (port2 = 47750; port2 <= 47799; port2++) {
                lfd4b = dyn_aio_listen(a, "127.0.0.1", (uint16_t)port2, 4);
                if (lfd4b >= 0)
                    break;
            }
            CHECK(lfd4b >= 0, "no free port in 47750..47799 for the retry case");
            if (lfd4b >= 0) {
                CHECK(dyn_aio_accept(a, lfd4b, on_accept, (void *)0xA5) == 0,
                      "accept retry-v4");
                c4_calls = 0; c4_res = 0; acc_count = 0; acc_fd = -1;
                cfd = dyn_aio_connect(a, "localhost", (uint16_t)port2,
                                      on_conn4, (void *)0xC4);
                CHECK(cfd >= 0, "retry-case connect returned an fd (got %d)",
                      cfd);
                pump(a, &c4_calls, PUMP_MAX);
                CHECK(c4_calls == 1, "retry-case connect completed %d times, "
                      "want 1 (the refused ::1 dial plus one retry is ONE "
                      "completion)", c4_calls);
                CHECK(c4_res == 0,
                      "second resolver answer must win: res=%d "
                      "(first-answer-wins code reports -ECONNREFUSED)",
                      c4_res);
                pump_accept(a, 1, 200);
                CHECK(acc_count >= 1,
                      "the v4 listener never saw the retried connection");
                if (cfd >= 0)
                    dyn_aio_close(a, cfd);
                if (acc_fd >= 0)
                    close(acc_fd);
                dyn_aio_cancel(a, on_accept, (void *)0xA5);
                dyn_aio_close(a, lfd4b);
            }
        }
    }

    /* ---- teardown: nothing armed, nothing in flight ---- */
    dyn_aio_cancel(a, on_accept, (void *)0xA4);
    dyn_aio_cancel(a, on_accept, (void *)0xA6);
    /* let any straggler dispatches land before the reactor goes away */
    for (spins = 0; spins < 10; spins++)
        dyn_aio_run(a, 5);
    dyn_aio_close(a, lfd4);
    dyn_aio_close(a, lfd6);
    dyn_aio_free(a);

    if (fails == 0) printf("test_aio_connect_offload: all tests passed\n");
    else printf("test_aio_connect_offload: %d FAILED\n", fails);
    return fails != 0;
}

#else /* no native modules: no dyn_aio at all */
int main(void)
{
    printf("test_aio_connect_offload: skipped (CONFIG_NATIVE_MODULES off)\n");
    return 0;
}
#endif
